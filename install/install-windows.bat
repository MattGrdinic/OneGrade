@echo off
REM OneGrade - Windows installer. Right-click > Run as administrator.
REM Copies OneGrade.ofx.bundle into the common OFX plugin folder.
setlocal
set "SRC=%~dp0OneGrade.ofx.bundle"
set "DEST=%CommonProgramFiles%\OFX\Plugins"

if not exist "%SRC%" (
  echo OneGrade.ofx.bundle not found next to this installer.
  pause & exit /b 1
)

net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Please right-click this file and choose "Run as administrator".
  pause & exit /b 1
)

if not exist "%DEST%" mkdir "%DEST%"
if exist "%DEST%\OneGrade.ofx.bundle" rmdir /s /q "%DEST%\OneGrade.ofx.bundle"
REM Remove the old PowerGrade bundle - same plugin, renamed. Leaving both installed
REM just shows a dead duplicate. Grades saved with PowerGrade will NOT carry over.
if exist "%DEST%\PowerGrade.ofx.bundle" rmdir /s /q "%DEST%\PowerGrade.ofx.bundle"
xcopy /E /I /Y "%SRC%" "%DEST%\OneGrade.ofx.bundle" >nul

echo OneGrade installed to "%DEST%".
echo Restart DaVinci Resolve, then find it under Effects ^> OpenFX ^> OneGrade.
pause
