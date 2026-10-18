#!/bin/bash
# Fails if EXE, a Windows build of saturn, imports a DLL of Microsoft's C++ redistributable, which a fresh
# Windows lacks, or has no manifest making UTF-8 its code page.
set -euo pipefail
exe=$1
imports=$("${OBJDUMP:-llvm-objdump}" -p "$exe")
grep -q "DLL Name: KERNEL32.dll" <<< "$imports"
if grep -iE "DLL Name: (vcruntime|msvcp)" <<< "$imports"; then
    echo "$exe needs Microsoft's C++ redistributable"
    exit 1
fi
if ! grep -q -a "<activeCodePage[^>]*>UTF-8</activeCodePage>" "$exe"; then
    echo "$exe has no manifest making UTF-8 its code page"
    exit 1
fi
