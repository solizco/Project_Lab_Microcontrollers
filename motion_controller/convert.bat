@echo off
REM ---------------------------------------------------------------
REM Converts every .gif in gifs\ into JPEG frames under data\.
REM
REM Category comes from the filename with trailing digits removed:
REM   idle.gif      -> data\idle\0
REM   idle1.gif     -> data\idle\1
REM   good_move.gif -> data\good_move\0
REM
REM Save in random_esp32_testing\ and double-click, or run convert.bat
REM from that folder.
REM
REM The -pix_fmt and -huffman flags matter: JPEGDEC on the ESP32 needs
REM 4:2:0 chroma and standard Huffman tables. Without them the decoder
REM returns error 2 and the screen shows blocky garbage.
REM ---------------------------------------------------------------

set FPS=15
set SIZE=320:240
set QUALITY=8
REM Seconds kept from each gif. Set to 0 to keep them whole.
set MAXLEN=2

if not exist gifs (
  echo No gifs folder here. Run this from random_esp32_testing.
  pause
  exit /b
)

set TRIM=
if not "%MAXLEN%"=="0" set TRIM=-t %MAXLEN%

echo Clearing old data folder...
if exist data rd /s /q data
mkdir data

for %%G in (gifs\*.gif) do call :one "%%G"

echo.
echo === Results ===
for /d %%C in (data\*) do (
  for /d %%V in ("%%C\*") do (
    for /f %%K in ('dir /b "%%V\*.jpg" 2^>nul ^| find /c /v ""') do echo   %%~nxC\%%~nxV: %%K frames
  )
)
echo.
powershell -nologo -command "'Total: {0:N2} MB  (keep under 1.8)' -f ((Get-ChildItem data -Recurse -File | Measure-Object Length -Sum).Sum/1MB)"
echo.
echo Now run:  platformio.exe run --target uploadfs --environment upesy_wroom
pause
exit /b

REM ---------------------------------------------------------------
:one
set FILE=%~1
set NAME=%~n1

:strip
echo %NAME%| findstr /r "[0-9]$" >nul
if %errorlevel%==0 (
  set NAME=%NAME:~0,-1%
  goto strip
)

if "%NAME%"=="" (
  echo SKIPPING %FILE% - filename is only digits
  exit /b
)

set V=0
:slot
if exist "data\%NAME%\%V%" (
  set /a V+=1
  goto slot
)

mkdir "data\%NAME%\%V%"
echo Converting %~nx1 -^> data\%NAME%\%V%
ffmpeg -hide_banner -loglevel error -y %TRIM% -i "%FILE%" -vf "fps=%FPS%,scale=%SIZE%:force_original_aspect_ratio=increase,crop=%SIZE%" -q:v %QUALITY% -pix_fmt yuvj420p -huffman default -start_number 0 "data\%NAME%\%V%\%%03d.jpg"
exit /b
