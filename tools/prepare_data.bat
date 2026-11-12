@echo off
rem Zombiewood Vita: build the ux0:data\zombiewood folder from your own APK and OBB files.
rem Put this file, prepare_data.py, your APK and both .obb files in one folder, then double-click it.
rem Needs Python 3 from python.org (tick "Add python.exe to PATH" when installing).
cd /d "%~dp0"
set APK=
for %%f in (*.apk) do set APK=%%f
if "%APK%"=="" (
  echo No .apk file found next to this script.
  pause
  exit /b 1
)
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 prepare_data.py --apk "%APK%" --obb-dir . --out zombiewood
) else (
  python prepare_data.py --apk "%APK%" --obb-dir . --out zombiewood
)
pause
