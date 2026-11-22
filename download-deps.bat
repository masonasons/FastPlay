@echo off
REM Download dependencies for FastPlay
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ============================================
echo FastPlay Dependency Downloader
echo ============================================
echo.

REM Check for git
where git >nul 2>&1
if errorlevel 1 (
    echo Error: Git is not installed or not in PATH.
    echo Please install Git from https://git-scm.com/
    exit /b 1
)

REM Create directories (lib\ receives the screen reader client DLLs at build time)
if not exist "lib" mkdir lib
if not exist "deps" mkdir deps

REM Temporary download folder
if not exist "temp_dl" mkdir temp_dl

echo Downloading SQLite...
powershell -Command "Invoke-WebRequest -Uri 'https://sqlite.org/2026/sqlite-amalgamation-3510200.zip' -OutFile 'temp_dl\sqlite.zip'"
rem tar (in Windows since 10) rather than Expand-Archive, which breaks when Windows
rem PowerShell loads PowerShell 7's Archive module
if not exist "temp_dl\sqlite" mkdir "temp_dl\sqlite"
tar -xf "temp_dl\sqlite.zip" -C "temp_dl\sqlite"
copy /y "temp_dl\sqlite\sqlite-amalgamation-3510200\sqlite3.c" "src\" >nul
if not exist "src\sqlite3.c" (
    echo Error: SQLite could not be downloaded or unpacked.
    exit /b 1
)

echo.
echo Cloning Speedy (Google's nonlinear speech speedup)...
if exist "deps\speedy" rmdir /s /q "deps\speedy"
git clone --depth 1 https://github.com/google/speedy.git "deps\speedy"

echo.
echo Cloning Signalsmith Stretch...
if exist "deps\signalsmith-stretch" rmdir /s /q "deps\signalsmith-stretch"
git clone --depth 1 https://github.com/Signalsmith-Audio/signalsmith-stretch.git "deps\signalsmith-stretch"
REM Also clone signalsmith-linear which is a separate dependency
git clone --depth 1 https://github.com/Signalsmith-Audio/linear.git "deps\signalsmith-stretch\signalsmith-linear"

echo.
echo Cloning Sonic...
if exist "deps\sonic" rmdir /s /q "deps\sonic"
git clone --depth 1 https://github.com/waywardgeek/sonic.git "deps\sonic"

echo.
echo Cloning KissFFT...
if exist "deps\kissfft" rmdir /s /q "deps\kissfft"
git clone --depth 1 https://github.com/mborgerding/kissfft.git "deps\kissfft"

echo.
echo Cloning FDK AAC (for xHE-AAC)...
if exist "deps\fdk-aac" rmdir /s /q "deps\fdk-aac"
git clone --depth 1 https://github.com/mstorsjo/fdk-aac.git "deps\fdk-aac"

REM FFmpeg: FastPlay's audio-only build (ci\ffmpeg). CI builds it in the job; a local
REM build takes CI's latest, with the GitHub CLI (gh auth login first). To build it
REM yourself instead: ci\ffmpeg\build.sh windows ffmpeg, in MSYS2 with the MSVC tools.
if /i not "%CI%"=="true" (
    echo.
    echo Downloading FFmpeg from CI...
    where gh >nul 2>&1
    if errorlevel 1 (
        echo Warning: the GitHub CLI ^(gh^) is not installed, so FFmpeg was not downloaded.
    ) else (
        set "RUNID="
        for /f "usebackq delims=" %%i in (`gh run list --repo masonasons/FastPlay --workflow build.yml --status success --limit 20 --json databaseId --jq ".[].databaseId"`) do (
            if not defined RUNID (
                gh run download %%i --repo masonasons/FastPlay --name ffmpeg-windows --dir temp_dl\ffmpeg >nul 2>&1
                if exist "temp_dl\ffmpeg\lib\avformat.lib" set "RUNID=%%i"
            )
        )
        if defined RUNID (
            if exist "ffmpeg" rmdir /s /q "ffmpeg"
            xcopy /e /i /q /y "temp_dl\ffmpeg" "ffmpeg" >nul
            echo FFmpeg from CI run !RUNID!.
        ) else (
            echo Warning: no FFmpeg build was found in CI's recent runs.
        )
    )
)

echo.
echo Cleaning up...
rmdir /s /q temp_dl 2>nul

echo.
echo ============================================
echo Download and build complete!
echo ============================================
echo.
echo Run build_new.bat to compile FastPlay. CMake fetches wxWidgets, miniaudio and
echo UniversalSpeech itself on the first build.
echo.
