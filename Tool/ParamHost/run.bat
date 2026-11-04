@echo off
setlocal
cd /d "%~dp0"

rem ---------------------------------------------------------------------------
rem  This file MUST stay pure ASCII.
rem  cmd.exe reads a .bat byte-by-byte in the OEM codepage; mixing UTF-8 Chinese
rem  and "chcp 65001" makes cmd resume at the wrong file offset and it will try
rem  to execute fragments such as "et", "f", "ause".
rem ---------------------------------------------------------------------------

set "PY="
py -3.12 -c "import PySide6, serial" >nul 2>nul
if not errorlevel 1 set "PY=py -3.12"

if not defined PY (
    py -3 -c "import PySide6, serial" >nul 2>nul
    if not errorlevel 1 set "PY=py -3"
)
if not defined PY (
    python -c "import PySide6, serial" >nul 2>nul
    if not errorlevel 1 set "PY=python"
)
if not defined PY goto nodep

echo [host] interpreter: %PY%
%PY% qt_main.py %*
if errorlevel 1 pause
exit /b 0

:nodep
echo.
echo  [!] No Python interpreter found that has BOTH PySide6 and pyserial.
echo.
echo  python found in PATH:
where python 2>nul
where py 2>nul
echo.
echo  How to fix:
echo    py -3 -m pip install PySide6 pyserial
echo    (optional, real Fluent widgets)  py -3 -m pip install PySide6-Fluent-Widgets
echo.
echo  Diagnose:  py -3 qt_main.py --check
echo.
pause
exit /b 1
