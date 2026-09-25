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
    if MARK in text:
        print(f"already patched: {rel}")
        return
    new = edit(text)
    if new == text:
        sys.exit(f"patch-fex-ios: anchor not found in {rel}")
    path.write_text(new)
    print(f"patched: {rel}")


def insert_before(text, anchor, block):
    if text.count(anchor) != 1:
        return text
    return text.replace(anchor, block + anchor)


# On Apple, FEX's CMake forces ENABLE_FEX_ALLOCATOR off, but the fallback
# malloc_usable_size still calls IOS_RPM_GUARD(), which is only defined in the
# allocator branch.
patch("FEXCore/Source/Utils/AllocatorHooks.cpp",
      lambda t: f"#ifndef ENABLE_FEX_ALLOCATOR {MARK}\n#define IOS_RPM_GUARD() ((void)0)\n#endif\n" + t)


# The [ffs-bypass] / [cb-entry] reporters in CompileBlock read IosFfsBypassLog
# and IosCbEntryLog, which are declared (and defined) only under FEX_IOS_HOST.
def core(t):
    t = insert_before(t, "  /* iOS-Madeira ml304 (task #51): REPORT CallbackPtr ENTRY ON ITS OWN",
                      f"#ifdef FEX_IOS_HOST {MARK}\n")
    t2 = insert_before(t, "  /* iOS-Madeira: refuse to compile obviously-invalid guest RIPs.",
                       "#endif\n\n")
    return t2 if t2 != t and MARK in t else t


patch("FEXCore/Source/Interface/Core/Core.cpp", core)
