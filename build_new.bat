@echo off
REM Build script for FastPlay with modular source files

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM Find Visual Studio using vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Error: Cannot find vswhere.exe - Visual Studio 2017+ required
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if not defined VSINSTALL (
    echo Error: Cannot find Visual Studio with C++ tools
    exit /b 1
)

REM Set up the environment for x64
if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" (
    call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else (
    echo Error: Cannot find vcvars64.bat
    exit /b 1
)

REM Default build flags (Universal Speech + Speedy enabled by default)
set "SPEECH_FLAG=/DUSE_UNIVERSAL_SPEECH /DUNIVERSAL_SPEECH_STATIC"
set "SPEECH_LIBS=UniversalSpeechStatic.lib ole32.lib oleaut32.lib version.lib psapi.lib"
set "SPEEDY_FLAG=/DUSE_SPEEDY /DKISS_FFT /DSONIC_INTERNAL"
set "SPEEDY_INC=/I"deps\speedy" /I"deps\sonic" /I"deps\kissfft""
set "SPEEDY_SRC=deps\speedy\speedy.c deps\speedy\soniclib.c deps\sonic\sonic.c deps\kissfft\kiss_fft.c"
set "SIGNALSMITH_FLAG=/DUSE_SIGNALSMITH"
set "SIGNALSMITH_INC=/I"deps\signalsmith-stretch""
set "STEAMAUDIO_FLAG=/DUSE_STEAM_AUDIO"
set "STEAMAUDIO_INC=/I"deps\steamaudio\include""
set "STEAMAUDIO_LIB="

REM Parse arguments
:parse_args
if "%1"=="" goto :done_args
if "%1"=="no-speech" (
    set "SPEECH_FLAG="
    set "SPEECH_LIBS="
    echo Disabling screen reader support...
) else if "%1"=="no-steamaudio" (
    set "STEAMAUDIO_FLAG="
    set "STEAMAUDIO_INC="
    set "STEAMAUDIO_LIB="
    echo Disabling Steam Audio support...
)
shift
goto :parse_args
:done_args

REM Read version from version.h
set "APP_VERSION="
for /f "tokens=3 delims= " %%v in ('findstr /C:"#define APP_VERSION " include\fastplay\version.h') do set "APP_VERSION=%%~v"

REM Get git commit hash for update-check comparison (short SHA, e.g. "ad07165")
set "BUILD_COMMIT="
for /f "tokens=*" %%i in ('git rev-parse --short HEAD 2^>nul') do set "BUILD_COMMIT=%%i"
if defined BUILD_COMMIT (
    set "COMMIT_FLAG=/DBUILD_COMMIT=\"%BUILD_COMMIT%\""
    echo Building FastPlay %APP_VERSION% ^(commit %BUILD_COMMIT%^)...
) else (
    set "COMMIT_FLAG="
    echo Building FastPlay %APP_VERSION%...
)

REM Source files
set "SOURCES=src\main.cpp src\globals.cpp src\utils.cpp src\player.cpp"
set "SOURCES=%SOURCES% src\settings.cpp src\hotkeys.cpp src\tray.cpp"
set "SOURCES=%SOURCES% src\accessibility.cpp src\ui.cpp src\effects.cpp"
set "SOURCES=%SOURCES% src\database.cpp src\sqlite3.c"
set "SOURCES=%SOURCES% src\tempo_processor.cpp src\youtube.cpp src\center_cancel.cpp src\convolution.cpp src\download_manager.cpp src\updater.cpp src\spatial_audio.cpp"

REM Add Speedy source if enabled
if defined SPEEDY_SRC set "SOURCES=%SOURCES% %SPEEDY_SRC%"

REM Compile resources
rc /nologo FastPlay.rc
if errorlevel 1 goto :error

REM Compile and link
cl /nologo /W3 /O2 /MT /EHsc /DUNICODE /D_UNICODE /DNOMINMAX %COMMIT_FLAG% %SPEECH_FLAG% %SPEEDY_FLAG% %SIGNALSMITH_FLAG% %STEAMAUDIO_FLAG% ^
   /I"." /I"include" /I"include\fastplay" %SPEEDY_INC% %SIGNALSMITH_INC% %STEAMAUDIO_INC% ^
   %SOURCES% FastPlay.res ^
   /Fe:FastPlay.exe ^
   /link /LIBPATH:"lib" /DELAYLOAD:bass.dll /DELAYLOAD:bass_fx.dll /DELAYLOAD:bass_aac.dll /DELAYLOAD:bassmidi.dll /DELAYLOAD:bassenc.dll /DELAYLOAD:bassenc_mp3.dll /DELAYLOAD:bassenc_ogg.dll /DELAYLOAD:bassenc_flac.dll ^
   bass.lib bass_fx.lib bass_aac.lib bassmidi.lib bassenc.lib bassenc_mp3.lib bassenc_ogg.lib bassenc_flac.lib %SPEECH_LIBS% user32.lib comctl32.lib comdlg32.lib shell32.lib shlwapi.lib advapi32.lib ole32.lib delayimp.lib

if errorlevel 1 goto :error

REM Clean up intermediate files
del /q *.obj *.res 2>nul

REM DLLs are loaded from lib subfolder via SetDllDirectory, no copy needed

REM Build distribution zip
echo Building distribution...
call "%~dp0dist.bat"

REM Build installer if Inno Setup is available
set "ISCC="
for %%V in (7 6) do (
    if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup %%V\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup %%V\ISCC.exe"
    if not defined ISCC if exist "%ProgramFiles%\Inno Setup %%V\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup %%V\ISCC.exe"
)

if defined ISCC (
    echo Building installer...
    REM Prepare dist_temp for installer
    if exist "dist_temp" rmdir /s /q "dist_temp"
    mkdir "dist_temp"
    copy /y "FastPlay.exe" "dist_temp\" >nul
    mkdir "dist_temp\docs" 2>nul
    xcopy /y /e "docs\*" "dist_temp\docs\" >nul 2>&1
    mkdir "dist_temp\lib" 2>nul
    for %%f in (lib\*.dll) do copy /y "%%f" "dist_temp\lib\" >nul 2>&1
    "%ISCC%" /DMyAppVersion=%APP_VERSION% /DSourceDir=dist_temp /DOutputDir=. installer.iss
    if errorlevel 1 (
        echo Installer build failed!
    ) else (
        echo Installer built successfully: FastPlayInstaller.exe
    )
    rmdir /s /q "dist_temp" 2>nul
) else (
    echo Inno Setup not found, skipping installer build.
)

echo.
echo Build successful! Run FastPlay.exe to start.
goto :end

:error
echo.
echo Build failed!
exit /b 1

:end
