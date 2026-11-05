@echo off
setlocal
cd /d "%~dp0"

rem ---------------------------------------------------------------------------
rem  Build a single-file exe of the Qt6 host tool (dist\MotorParamHost.exe).
rem  This file MUST stay pure ASCII + CRLF (cmd reads it byte-by-byte).
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

echo [build] interpreter: %PY%
echo [build] checking runtime requirements ...
%PY% -m pip install --disable-pip-version-check -r requirements.txt
%PY% -m pip show pyinstaller >nul 2>nul
if errorlevel 1 (
    echo [build] installing pyinstaller ...
    %PY% -m pip install --disable-pip-version-check -r requirements-dev.txt
)

echo [build] packaging onefile exe (this may take a few minutes) ...
%PY% -m PyInstaller --noconfirm --clean --onefile --windowed ^
  --name MotorParamHost ^
  --distpath dist --workpath build\pyinstaller --specpath build ^
  --collect-all qfluentwidgets ^
  --exclude-module PySide6.QtWebEngineCore ^
  --exclude-module PySide6.QtWebEngineWidgets ^
  --exclude-module PySide6.QtWebEngineQuick ^
  --exclude-module PySide6.QtWebChannel ^
  --exclude-module PySide6.QtWebSockets ^
  --exclude-module PySide6.Qt3DCore ^
  --exclude-module PySide6.Qt3DRender ^
  --exclude-module PySide6.Qt3DInput ^
  --exclude-module PySide6.Qt3DLogic ^
  --exclude-module PySide6.Qt3DAnimation ^
  --exclude-module PySide6.Qt3DExtras ^
  --exclude-module PySide6.QtCharts ^
  --exclude-module PySide6.QtDataVisualization ^
  --exclude-module PySide6.QtGraphs ^
  --exclude-module PySide6.QtGraphsWidgets ^
  --exclude-module PySide6.QtMultimedia ^
  --exclude-module PySide6.QtMultimediaWidgets ^
  --exclude-module PySide6.QtQuick ^
  --exclude-module PySide6.QtQml ^
  --exclude-module PySide6.QtQuickWidgets ^
  --exclude-module PySide6.QtQuick3D ^
  --exclude-module PySide6.QtPdf ^
  --exclude-module PySide6.QtPdfWidgets ^
  --exclude-module PySide6.QtPositioning ^
  --exclude-module PySide6.QtLocation ^
  --exclude-module PySide6.QtBluetooth ^
  --exclude-module PySide6.QtNfc ^
  --exclude-module PySide6.QtSensors ^
  --exclude-module PySide6.QtSerialPort ^
  --exclude-module PySide6.QtSql ^
  --exclude-module PySide6.QtTest ^
  --exclude-module PySide6.QtHelp ^
  --exclude-module PySide6.QtDesigner ^
  --exclude-module PySide6.QtUiTools ^
  --exclude-module PySide6.QtTextToSpeech ^
  --exclude-module PySide6.QtSpatialAudio ^
  --exclude-module PySide6.QtNetworkAuth ^
  --exclude-module PySide6.QtRemoteObjects ^
  --exclude-module PySide6.QtScxml ^
  --exclude-module PySide6.QtStateMachine ^
  qt_main.py

if errorlevel 1 goto failed

echo.
echo [build] OK -^> dist\MotorParamHost.exe
dir /b dist\MotorParamHost.exe
exit /b 0

:failed
echo.
echo [build] FAILED - see the messages above
pause
exit /b 1

:nodep
echo.
echo  [!] No Python interpreter found that has BOTH PySide6 and pyserial.
echo      Install them first:
echo        py -3 -m pip install -r requirements.txt
echo        py -3 -m pip install -r requirements-dev.txt
echo.
pause
exit /b 1
