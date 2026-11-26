@echo off
echo ========================================================
echo Building av_coop.dll (x64) - Modded by GreatMan
echo ========================================================
windres av_coop.rc -O coff -o av_coop_res.o
gcc -O2 -Wall -shared -s -o ..\av_coop.dll av_coop64.c av_coop_res.o -lkernel32 -luser32
if %ERRORLEVEL% EQU 0 (
    echo [OK] av_coop.dll built successfully!
) else (
    echo [FAIL] Compilation error.
)
pause
