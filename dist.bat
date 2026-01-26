@echo off
REM Distribution script for FastPlay - creates a zip archive

setlocal
cd /d "%~dp0"

REM Check if FastPlay.exe exists
if not exist "FastPlay.exe" (
    echo Error: FastPlay.exe not found. Run build_new.bat first.
    exit /b 1
)

REM Check if required DLLs exist in lib folder
if not exist "lib\bass.dll" (
    echo Error: lib\bass.dll not found.
    exit /b 1
)
if not exist "lib\bass_fx.dll" (
    echo Error: lib\bass_fx.dll not found.
    exit /b 1
)

REM Set output filename
set "ZIPNAME=FastPlay.zip"

REM Remove old zip if exists
if exist "%ZIPNAME%" del "%ZIPNAME%"

REM Create a temporary folder for distribution files
if exist "dist_temp" rmdir /s /q "dist_temp"
mkdir "dist_temp"

REM Copy exe and docs to temp folder
copy /y "FastPlay.exe" "dist_temp\"
mkdir "dist_temp\docs"
xcopy /y /e "docs\*" "dist_temp\docs\" >nul

REM Create lib subfolder and copy all DLLs
echo Copying DLLs to lib folder...
mkdir "dist_temp\lib"
copy /y "lib\bass.dll" "dist_temp\lib\"
copy /y "lib\bass_fx.dll" "dist_temp\lib\"
copy /y "lib\nvdaControllerClient64.dll" "dist_temp\lib\" 2>&1
copy /y "lib\SAAPI64.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassflac.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassopus.dll" "dist_temp\lib\" 2>&1
copy /y "lib\basswma.dll" "dist_temp\lib\" 2>&1
copy /y "lib\basswv.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassape.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassalac.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassmidi.dll" "dist_temp\lib\" 2>&1
copy /y "lib\basscd.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassdsd.dll" "dist_temp\lib\" 2>&1
copy /y "lib\basshls.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bass_aac.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassenc.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassenc_mp3.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassenc_ogg.dll" "dist_temp\lib\" 2>&1
copy /y "lib\bassenc_flac.dll" "dist_temp\lib\" 2>&1

REM Create zip using PowerShell
echo Creating %ZIPNAME%...
powershell -NoProfile -Command "Compress-Archive -Path 'dist_temp\*' -DestinationPath '%ZIPNAME%' -Force"

if errorlevel 1 (
    rmdir /s /q "dist_temp"
    echo Failed to create zip file.
    exit /b 1
)

REM Clean up temp folder
rmdir /s /q "dist_temp"

echo.
echo Distribution created: %ZIPNAME%
echo Contents:
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; $z = [System.IO.Compression.ZipFile]::OpenRead('%ZIPNAME%'); $z.Entries | ForEach-Object { Write-Host ('  ' + $_.Name + ' (' + [math]::Round($_.Length/1KB) + ' KB)') }; $z.Dispose()"
