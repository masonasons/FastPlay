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

REM Default build flags (Universal Speech + Rubber Band + Speedy enabled by default)
set "SPEECH_FLAG=/DUSE_UNIVERSAL_SPEECH /DUNIVERSAL_SPEECH_STATIC"
set "SPEECH_LIBS=UniversalSpeechStatic.lib ole32.lib oleaut32.lib version.lib psapi.lib"
set "RUBBERBAND_FLAG=/DUSE_RUBBERBAND /DRUBBERBAND_STATIC /DNOMINMAX"
set "RUBBERBAND_INC=/I"deps\rubberband-4.0.0""
set "RUBBERBAND_SRC=deps\rubberband-4.0.0\single\RubberBandSingle.cpp"
set "SPEEDY_FLAG=/DUSE_SPEEDY /DKISS_FFT /DSONIC_INTERNAL"
set "SPEEDY_INC=/I"deps\speedy" /I"deps\sonic" /I"deps\kissfft""
set "SPEEDY_SRC=deps\speedy\speedy.c deps\speedy\soniclib.c deps\sonic\sonic.c deps\kissfft\kiss_fft.c"

REM Parse arguments
:parse_args
if "%1"=="" goto :done_args
if "%1"=="no-speech" (
    set "SPEECH_FLAG="
    set "SPEECH_LIBS="
    echo Disabling screen reader support...
) else if "%1"=="no-rubberband" (
    set "RUBBERBAND_FLAG="
    set "RUBBERBAND_INC="
    set "RUBBERBAND_SRC="
    echo Disabling Rubber Band support...
)
shift
goto :parse_args
:done_args

echo Building FastPlay...

REM Source files
set "SOURCES=src\main.cpp src\globals.cpp src\utils.cpp src\player.cpp"
set "SOURCES=%SOURCES% src\settings.cpp src\hotkeys.cpp src\tray.cpp"
set "SOURCES=%SOURCES% src\accessibility.cpp src\ui.cpp src\effects.cpp"
set "SOURCES=%SOURCES% src\database.cpp src\sqlite3.c"
set "SOURCES=%SOURCES% src\tempo_processor.cpp src\youtube.cpp src\center_cancel.cpp src\convolution.cpp"

REM Add Rubber Band source if enabled
if defined RUBBERBAND_SRC set "SOURCES=%SOURCES% %RUBBERBAND_SRC%"

REM Add Speedy source if enabled
if defined SPEEDY_SRC set "SOURCES=%SOURCES% %SPEEDY_SRC%"

REM Compile resources
rc /nologo FastPlay.rc
if errorlevel 1 goto :error

REM Compile and link
cl /nologo /W3 /O2 /MT /EHsc /DUNICODE /D_UNICODE %SPEECH_FLAG% %RUBBERBAND_FLAG% %SPEEDY_FLAG% ^
   /I"." /I"include" /I"include\fastplay" %RUBBERBAND_INC% %SPEEDY_INC% ^
   %SOURCES% FastPlay.res ^
   /Fe:FastPlay.exe ^
   /link /LIBPATH:"lib" /DELAYLOAD:bass.dll /DELAYLOAD:bass_fx.dll /DELAYLOAD:bass_aac.dll /DELAYLOAD:bassmidi.dll /DELAYLOAD:bassenc.dll /DELAYLOAD:bassenc_mp3.dll /DELAYLOAD:bassenc_ogg.dll /DELAYLOAD:bassenc_flac.dll ^
   bass.lib bass_fx.lib bass_aac.lib bassmidi.lib bassenc.lib bassenc_mp3.lib bassenc_ogg.lib bassenc_flac.lib %SPEECH_LIBS% user32.lib comctl32.lib comdlg32.lib shell32.lib shlwapi.lib advapi32.lib ole32.lib delayimp.lib

if errorlevel 1 goto :error

REM Clean up intermediate files
del /q *.obj *.res 2>nul

REM DLLs are loaded from lib subfolder via SetDllDirectory, no copy needed

echo.
echo Build successful! Run FastPlay.exe to start.
goto :end

:error
echo.
echo Build failed!
exit /b 1

:end
