#!/bin/bash
# Runs steamservice-x64.exe under a desktop Wine against a sample install
# script and checks the registry it leaves behind. Needs wine (64-bit).
# Usage: build/steamservice/test.sh
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
EXE="$DIR/steamservice-x64.exe"
WINE="${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}"
export WINEPREFIX="${WINEPREFIX:-$(mktemp -d)/prefix}"
export WINEDEBUG=-all
fail=0

wine_reg() { "$WINE" reg "$@" 2>/dev/null | tr -d '\r'; }

check() {  # check <key> <value name> <expected substring>
    local out
    out="$(wine_reg query "$1" /v "$2" /reg:32)"
    if grep -qF -- "$3" <<<"$out"; then
        echo "ok    $1\\$2 has '$3'"
    else
        echo "FAIL  $1\\$2 expected '$3', got: $(grep -F -- "$2" <<<"$out" || echo '<missing>')"
        fail=1
    fi
}

absent() {  # absent <key> <value name> [view]
    if wine_reg query "$1" /v "$2" ${3:-} | grep -qF -- "$2"; then
        echo "FAIL  $1\\$2 should not exist"; fail=1
    else
        echo "ok    $1\\$2 absent"
    fi
}

"$WINE" wineboot -i >/dev/null 2>&1
C="$WINEPREFIX/drive_c"
SH="$C/Program Files (x86)/Steam/steamapps/common/Steamworks Shared"
mkdir -p "$SH"
wine_reg add 'HKCU\Software\Valve\Steam' /v Language /t REG_SZ /d russian /f >/dev/null
wine_reg add 'HKLM\Software\Test\Keep' /v Existing /t REG_SZ /d old /f /reg:32 >/dev/null

cat > "$SH/runasadmin.vdf" <<'EOF'
// Steam install script, admin part
"InstallScript"
{
	"Registry"
	{
		"HKEY_LOCAL_MACHINE\\Software\\Test\\Game"
		{
			"string"
			{
				"any"
				{
					"InstallPath"		"%INSTALLDIR%"
					"Quoted"		"say \"hi\""
					""			"default value"
				}
				"english"	{ "Language"	"en" }
				"russian"	{ "Language"	"ru" }
			}
			"dword"
			{
				"any"
				{
					"Installed"		"1"
					"Flags"			"0x20"
				}
			}
		}
		"HKEY_CURRENT_USER\\Software\\Test\\User"
		{
			"string" { "any" { "Docs" "%USER_MYDOCS%\\Game" } }
		}
	}
	"Registry If Not Present"
	{
		"HKEY_LOCAL_MACHINE\\Software\\Test\\Keep"
		{
			"string" { "any" { "Existing" "new"  "Fresh" "yes" } }
		}
	}
	"Run Process"
	{
		"DirectX"
		{
			"HasRunKey"		"HKEY_LOCAL_MACHINE\\Software\\Valve\\Steam\\Apps\\CommonRedist\\DirectX\\Jun2010"
			"process 1"		"%INSTALLDIR%\\_CommonRedist\\DirectX\\Jun2010\\DXSETUP.exe"
			"command 1"		"/silent"
			"NoCleanUp"		"1"
		}
		"vcredist 2022"	[$WIN64]
		{
			"HasRunKey"		"HKEY_LOCAL_MACHINE\\Software\\Valve\\Steam\\Apps\\CommonRedist\\vcredist\\2022"
			"RunKeyName"		"x64 14.40"
			"MinimumHasRunValue"	"2"
			"process 1"		"%INSTALLDIR%\\_CommonRedist\\vcredist\\2022\\VC_redist.x64.exe"
			"command 1"		"/install /quiet /norestart"
		}
		"Game Setup"
		{
			"process 1"		"%INSTALLDIR%\\setup.exe"
		}
	}
	"Firewall" { "Game" "%INSTALLDIR%\\game.exe" }
}
EOF

WIN_SCRIPT='C:\Program Files (x86)\Steam\steamapps\common\Steamworks Shared\runasadmin.vdf'
"$WINE" "$EXE" /installscript "$WIN_SCRIPT" 1118200 2>/dev/null
rc=$?
[ $rc -eq 0 ] && echo "ok    exit code 0" || { echo "FAIL  exit code $rc"; fail=1; }

K='HKLM\Software\Test\Game'
check "$K" InstallPath 'C:\Program Files (x86)\Steam\steamapps\common\Steamworks Shared'
check "$K" Quoted 'say "hi"'
check "$K" Language 'ru'
check "$K" Installed '0x1'
check "$K" Flags '0x20'
check "$K" '' 'default value'
absent "$K" InstallPath /reg:64
check 'HKLM\Software\Test\Keep' Existing 'old'
check 'HKLM\Software\Test\Keep' Fresh 'yes'
out="$(wine_reg query 'HKCU\Software\Test\User' /v Docs)"
grep -qiE 'Docs.*REG_SZ.*[A-Z]:\\.+\\Game' <<<"$out" && echo "ok    HKCU Docs expanded" || { echo "FAIL  HKCU Docs: $out"; fail=1; }
check 'HKLM\Software\Valve\Steam\Apps\CommonRedist\DirectX\Jun2010' DirectX '0x1'
check 'HKLM\Software\Valve\Steam\Apps\CommonRedist\vcredist\2022' 'x64 14.40' '0x2'
check 'HKLM\Software\Valve\Steam\Apps\1118200' 'Game Setup' '0x1'

LOG="$C/Program Files (x86)/Steam/logs/madeira_installscript.log"
[ -s "$LOG" ] && echo "ok    log written" || { echo "FAIL  no log at $LOG"; fail=1; }

printf '"InstallScript"\n{\n\t"Registry"\n\t{\n' > "$SH/broken.vdf"
"$WINE" "$EXE" /installscript "${WIN_SCRIPT%runasadmin.vdf}broken.vdf" 1 2>/dev/null
rc=$?
[ $rc -eq 0 ] && grep -q "not valid VDF" "$LOG" && echo "ok    truncated script -> logged, exit 0" \
    || { echo "FAIL  truncated script exit $rc"; fail=1; }

"$WINE" "$EXE" /uninstallscript whatever 2>/dev/null
rc=$?
[ $rc -eq 0 ] && echo "ok    other verbs -> exit 0" || { echo "FAIL  other verb exit $rc"; fail=1; }

echo "---- log ----"
cat "$LOG"
[ $fail -eq 0 ] && echo "ALL PASSED" || echo "SOME CHECKS FAILED"
exit $fail
