@echo off
rem Build usbcp.exe with MSVC (run from a "x64 Native Tools Command Prompt for VS")
rem or with MinGW-w64 if gcc is in PATH.

where cl >nul 2>nul
if %errorlevel%==0 (
    cl /nologo /W4 /O2 /utf-8 usbcp.c /Fe:usbcp.exe shell32.lib
    goto :done
)

where gcc >nul 2>nul
if %errorlevel%==0 (
    gcc -Wall -O2 -o usbcp.exe usbcp.c -lshell32 -static
    goto :done
)

echo Neither MSVC (cl.exe) nor MinGW (gcc) found in PATH.
echo Open "x64 Native Tools Command Prompt for VS" and run this script again.

:done
if exist usbcp.exe echo Build OK: usbcp.exe
