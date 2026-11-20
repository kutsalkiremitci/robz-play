@echo off
rem Robz Play - quick start. Double-click to launch the built application.
rem
rem A newer build that could not replace the running one waits in dist\RobzPlay-next;
rem it is moved into place here, as long as Robz Play is not running.
setlocal
set "APP_DIR=%~dp0dist\RobzPlay"
set "NEXT_DIR=%~dp0dist\RobzPlay-next"

tasklist /FI "IMAGENAME eq RobzPlay.exe" 2>nul | find /I "RobzPlay.exe" >nul
if not errorlevel 1 (
    echo Robz Play is already running.
    if exist "%NEXT_DIR%\RobzPlay.exe" echo An update is waiting: close Robz Play, then run this again to apply it.
    timeout /t 5 >nul
    exit /b 0
)

if exist "%NEXT_DIR%\RobzPlay.exe" (
    echo Applying the waiting update...
    robocopy "%NEXT_DIR%" "%APP_DIR%" /E /IS /MOVE /NFL /NDL /NJH /NJS /NP >nul
    if exist "%NEXT_DIR%" rmdir /S /Q "%NEXT_DIR%"
)

if not exist "%APP_DIR%\RobzPlay.exe" (
    echo Robz Play has not been built yet: "%APP_DIR%\RobzPlay.exe" is missing.
    echo Build it from an MSYS2 MINGW64 shell with: scripts/robzplay-build.sh
    pause
    exit /b 1
)
start "" /D "%APP_DIR%" "%APP_DIR%\RobzPlay.exe"
endlocal
