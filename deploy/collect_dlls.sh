#!/bin/bash
# Recursively copy the MinGW/Qt DLL dependencies of $1 into its directory.
# Windows system DLLs are skipped. Usage: collect_dlls.sh <path-to-exe>
#
# The Qt DLL location is taken from qmake(found on PATH), so this works for
# the official Qt binaries and for MSYS2's Qt alike.
set -u
QT_BIN=$(qmake -query QT_HOST_BINS 2>/dev/null)
target_dir=$(dirname "$1")
target_name=$(basename "$1")
cd "$target_dir" || exit 1
declare -A done_files

is_system_dll() {
    case "$1" in
        api-ms-*|ext-ms-*|KERNEL32.dll|msvcrt.dll|USER32.dll|SHELL32.dll|ADVAPI32.dll|\
        ole32.dll|OLEAUT32.dll|ntdll.dll|WS2_32.dll|WINMM.dll|VERSION.dll|USERENV.dll|\
        MPR.dll|NETAPI32.dll|AUTHZ.dll|GDI32.dll|gdi32.dll|d3d11.dll|dxgi.dll|\
        SETUPAPI.dll|cfgmgr32.dll|bcrypt.dll|CRYPT32.dll|crypt32.dll|Secur32.dll|\
        SHCORE.dll|combase.dll|RPCRT4.dll|dbghelp.dll|IMM32.dll|UxTheme.dll|uxtheme.dll|\
        DWMAPI.dll|dwmapi.dll|WSOCK32.dll|OpenGL32.dll|GLU32.dll|\
        WINTRUST.dll|MSIMG32.dll|WTSAPI32.dll|POWRPROF.dll|\
        IPHLPAPI.DLL|DNSAPI.dll|d3d12.dll|DWrite.dll|USP10.dll) return 0;;
        *) return 1;;
    esac
}

find_dll() {
    # look next to the exe first, then in the Qt bin dir, then on PATH
    if [ -f "$1" ]; then
        echo "$1"
    elif [ -n "$QT_BIN" ] && [ -f "$QT_BIN/$1" ]; then
        echo "$QT_BIN/$1"
    else
        command -v "$1" 2>/dev/null
    fi
}

collect() {
    local file="$1"
    [ -n "${done_files[$file]:-}" ] && return
    done_files[$file]=1
    local dll
    for dll in $(objdump -p "$file" 2>/dev/null | grep 'DLL Name' | awk '{print $3}'); do
        is_system_dll "$dll" && continue
        if [ ! -f "$dll" ]; then
            local src
            src=$(find_dll "$dll")
            if [ -n "$src" ]; then
                cp "$src" .
            else
                echo "WARN: $dll not found (dependency of $file)" >&2
                continue
            fi
        fi
        collect "$dll"                        # recurse into its dependencies
    done
}

collect "$target_name"
echo "==== collected DLLs ===="
ls -1 *.dll
