@echo off
rem scrdock build script: locate MSVC via vswhere (with fixed-path fallbacks),
rem compile scrdock.c, and optionally copy the exe to a target directory.
rem
rem usage: build.bat [target_dir]
rem   e.g.  build.bat D:\Test\Geek_Studio\extracted_files
setlocal
set "HERE=%~dp0"
set "OUT=%HERE%build"

rem ---- 1. locate vcvars64.bat -------------------------------------------
rem single-line ifs (no paren blocks): %ProgramFiles(x86)% contains parens
set "VCV="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSWOUT=%TEMP%\scrdock_vswhere.txt"
if exist "%VSWHERE%" "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSWOUT%" 2>nul
set "VSPATH="
if exist "%VSWOUT%" set /p VSPATH=<"%VSWOUT%"
if exist "%VSWOUT%" del "%VSWOUT%" 2>nul
if defined VSPATH if exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" set "VCV=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCV if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCV=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCV if exist "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCV=C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCV (
  echo [scrdock] error: vcvars64.bat not found via vswhere or fixed paths 1>&2
  exit /b 1
)

rem ---- 2. build ----------------------------------------------------------
if not exist "%OUT%" mkdir "%OUT%"
rem (vcvars64's internals emit a benign "vswhere.exe not found" on stderr; silence it)
call "%VCV%" >nul 2>nul
if errorlevel 1 exit /b 1
cd /d "%HERE%"
rc /nologo /fo "%OUT%\scrdock.res" scrdock.rc
if errorlevel 1 exit /b 1
cl /nologo /W4 /O2 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 ^
   scrdock.c "%OUT%\scrdock.res" /Fe:"%OUT%\scrdock.exe" /Fo:"%OUT%/" ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib kernel32.lib shell32.lib advapi32.lib
if errorlevel 1 exit /b 1

rem ---- 2b. drop build intermediates (full rebuild every time anyway) ----
del "%OUT%\scrdock.obj" "%OUT%\scrdock.res" 2>nul

rem ---- 3. optional deploy ------------------------------------------------
if not "%~1"=="" (
  copy /y "%OUT%\scrdock.exe" "%~1\" >nul
  echo [scrdock] copied to %~1
)
echo [scrdock] built %OUT%\scrdock.exe
