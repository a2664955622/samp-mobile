@echo off
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo Python not found on PATH. Install Python 3.10+ first.
  pause
  exit /b 1
)
python tools\packaging\menu.py %*
pause