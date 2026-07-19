@echo off
setlocal enabledelayedexpansion

rem ---------------------------------------------------------------
rem  AmiFetch disk builder
rem  Usage: makedisk.cmd [path\to\exe]   (default: out\AmiFetch.exe)
rem ---------------------------------------------------------------

rem -- check xdftool --
where xdftool >nul 2>nul
if errorlevel 1 (
    echo xdftool not found. Install with:  pip install amitools
    goto :fail
)

rem -- input exe --
set "EXE=out\AmiFetch.exe"
if not "%~1"=="" set "EXE=%~1"
if not exist "%EXE%" (
    echo %EXE% not found. Build first ^(F5 / gnumake^).
    goto :fail
)

rem -- menu --
echo.
echo  AmiFetch disk builder
echo  Input: %EXE%
echo.
echo  [1] Shrinkler-crunch the exe, then build the disk  (release)
echo  [2] Build the disk with the exe as-is              (quick test)
echo.
choice /c 12 /n /m "Pick 1 or 2: "
if errorlevel 2 goto :nocrunch

rem -- find shrinkler: PATH first, then the amiga-debug extension --
set "SHRINKLER="
where shrinkler >nul 2>nul
if not errorlevel 1 set "SHRINKLER=shrinkler"
if not defined SHRINKLER (
    for /d %%D in ("%USERPROFILE%\.vscode\extensions\bartmanabyss.amiga-debug-*") do (
        for /f "delims=" %%F in ('dir /s /b "%%D\Shrinkler.exe" 2^>nul') do set "SHRINKLER=%%F"
    )
)
if not defined SHRINKLER (
    echo Shrinkler not found on PATH or in the amiga-debug extension.
    echo Get it from https://github.com/askeksa/Shrinkler/releases
    goto :fail
)
echo Using Shrinkler: !SHRINKLER!

rem -- crunch: same flags as the extension's "slow" preset in amiga.json --
rem    -h = Amiga hunk executable, -f dff180 = flash background while
rem    decrunching, -9 = max compression
set "DISKEXE=out\AmiFetch.shr"
echo Crunching ^(this takes a moment^)...
"!SHRINKLER!" -h -f dff180 -9 "%EXE%" "%DISKEXE%"
if errorlevel 1 goto :fail
goto :builddisk

:nocrunch
set "DISKEXE=%EXE%"

:builddisk
set "ADF=AmiFetch.adf"
if exist "%ADF%" del "%ADF%"
xdftool "%ADF%" format "AmiFetch" ofs
if errorlevel 1 goto :fail
xdftool "%ADF%" boot install
if errorlevel 1 goto :fail
xdftool "%ADF%" write "%DISKEXE%" AmiFetch
if errorlevel 1 goto :fail
xdftool "%ADF%" write disk\AmiFetch.info AmiFetch.info
if errorlevel 1 goto :fail
xdftool "%ADF%" write disk\Disk.info Disk.info
if errorlevel 1 goto :fail
xdftool "%ADF%" makedir s
if errorlevel 1 goto :fail
xdftool "%ADF%" write disk\startup-sequence s/startup-sequence
if errorlevel 1 goto :fail

echo.
xdftool "%ADF%" list
echo.
echo Done: %ADF%  ^(contains %DISKEXE%^)
pause
exit /b 0

:fail
echo.
echo Build FAILED.
pause
exit /b 1