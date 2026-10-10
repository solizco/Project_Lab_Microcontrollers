@echo off
rem Double-click to convert everything in the gifs folder into the faces folder.
rem Installs pygame and pillow the first time if they are missing.
cd /d "%~dp0"

set PY=py
py -c "import sys" >nul 2>nul
if not %errorlevel%==0 set PY=python

%PY% -c "import pygame, PIL" >nul 2>nul
if not %errorlevel%==0 (
    echo Installing pygame and pillow for this Python, one time only...
    %PY% -m pip install pygame pillow
    %PY% -c "import pygame, PIL" >nul 2>nul
    if errorlevel 1 (
        echo.
        echo pygame did not install. Trying pygame-ce, which works the same way...
        %PY% -m pip install pygame-ce pillow
    )
    echo.
)

%PY% convert_gifs.py %*
echo.
pause
