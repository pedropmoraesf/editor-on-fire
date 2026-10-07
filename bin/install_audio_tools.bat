@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_audio_tools.ps1"
set ERR=%ERRORLEVEL%
echo.
if not "%ERR%"=="0" (
  echo Audio tools installation failed. Error code: %ERR%
) else (
  echo Audio tools installation completed successfully.
)
pause
exit /b %ERR%
