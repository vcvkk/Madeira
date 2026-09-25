#!/usr/bin/env python3
"""CI-only fixes that let the FEX fork's native iOS FEXCore build from a clean
checkout (build/fex-ios/build.sh). The fork's iOS work is exercised mainly
through the ARM64EC build (FEX_IOS_HOST), and a few FEX_IOS_HOST-only
references leaked into code the native build also compiles. Each patch is
idempotent and fails loudly if its anchor is gone, so a fork update that fixes
the source makes the patch obsolete rather than silently wrong."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2] / "FEX"
MARK = "/* CI: patch-fex-ios */"


def patch(rel, edit):
    path = ROOT / rel
    text = path.read_text()
    tag = f"patch-fex-ios:{edit.__name__}"
    if tag in text:
        print(f"already patched: {rel} ({edit.__name__})")
        return
    new = edit(text)
    if new == text:
        sys.exit(f"patch-fex-ios: anchor not found in {rel}")
    path.write_text(new.replace(MARK, f"/* CI: {tag} */", 1))
    print(f"patched: {rel} ({edit.__name__})")


def insert_before(text, anchor, block):
    if text.count(anchor) != 1:
        return text
    return text.replace(anchor, block + anchor)


# On Apple, FEX's CMake forces ENABLE_FEX_ALLOCATOR off, but the fallback
# malloc_usable_size still calls IOS_RPM_GUARD(), which is only defined in the
# allocator branch.
def rpm_guard(t):
    return f"#ifndef ENABLE_FEX_ALLOCATOR {MARK}\n#define IOS_RPM_GUARD() ((void)0)\n#endif\n" + t


patch("FEXCore/Source/Utils/AllocatorHooks.cpp", rpm_guard)


# The [ffs-bypass] / [cb-entry] reporters in CompileBlock read IosFfsBypassLog
# and IosCbEntryLog, which are declared (and defined) only under FEX_IOS_HOST.
def core(t):
    t = insert_before(t, "  /* iOS-Madeira ml304 (task #51): REPORT CallbackPtr ENTRY ON ITS OWN",
                      f"#ifdef FEX_IOS_HOST {MARK}\n")
    t2 = insert_before(t, "  /* iOS-Madeira: refuse to compile obviously-invalid guest RIPs.",
                       "#endif\n\n")
    return t2 if t2 != t and MARK in t2 else t


patch("FEXCore/Source/Interface/Core/Core.cpp", core)


# IosLogUnimplementedCASPAL describes the faulting region with VirtualQuery,
# a Win32 API that only exists in the ARM64EC PE build.
def caspal(t):
    start = "  MEMORY_BASIC_INFORMATION mbi {};\n"
    end = "                    mbi.Protect, type, mbi.State);\n"
    if t.count(start) != 1 or t.count(end) != 1:
        return t
    t = t.replace(start, f"#ifdef _WIN32 {MARK}\n" + start)
    return t.replace(end, end + "#else\n"
                     "  LogMan::Msg::EFmt(\"[caspal128] MISALIGNED-UNSUPPORTED Size={} addrReg=x{} addr={:#x} misalign={}\",\n"
                     "                    Size, AddressReg, GPRs[AddressReg], GPRs[AddressReg] & 15);\n"
                     "#endif\n")


patch("FEXCore/Source/Utils/ArchHelpers/Arm64.cpp", caspal)


# CompileBlock's [rpm-cas] probe calls rpm_cas_snapshot_take, which the fork's
# rpmalloc defines; the native iOS build has no rpmalloc (allocator forced off
# on Apple), so report "no snapshot".
def rpm(t):
    anchor = "int rpm_cas_snapshot_take(struct rpm_cas_snapshot* out);\n"
    if t.count(anchor) != 1:
        return t
    return t.replace(anchor, anchor + f"#ifndef FEX_IOS_HOST {MARK}\n"
                     "int rpm_cas_snapshot_take(struct rpm_cas_snapshot*) { return 0; }\n#endif\n")


patch("FEXCore/Source/Interface/Core/Core.cpp", rpm)
