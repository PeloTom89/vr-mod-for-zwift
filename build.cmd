@echo off
rem Builds zvr_loader.exe + zwift_vr.dll into build\Release\ with the VS 2026 Build Tools.
setlocal
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (echo VS 2026 Build Tools not found at "%VCVARS%" & exit /b 1)
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0"
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 || exit /b 1
rem A running Zwift keeps zwift_vr.dll loaded, so it can't be overwritten, but Windows does allow
rem renaming a loaded DLL. Move it aside so the build never has to wait for Zwift to close; old
rem copies are deleted on later builds once no Zwift has them loaded.
set "OUT=build\Release"
del /q "%OUT%\zwift_vr.dll.old-*" >nul 2>&1
if exist "%OUT%\zwift_vr.dll" del /q "%OUT%\zwift_vr.dll" >nul 2>&1
if exist "%OUT%\zwift_vr.dll" move /y "%OUT%\zwift_vr.dll" "%OUT%\zwift_vr.dll.old-%RANDOM%%RANDOM%" >nul
cmake --build build --config Release -- /m /nologo /v:minimal || goto :failed
echo.
echo Built: %~dp0build\Release\zvr_loader.exe + zwift_vr.dll
exit /b 0

:failed
rem Don't leave build\Release without a DLL: copy the newest moved-aside one back (copying a loaded
rem DLL is fine; only overwriting/deleting it is blocked).
if not exist "%OUT%\zwift_vr.dll" (
  for /f "delims=" %%F in ('dir /b /o-d "%OUT%\zwift_vr.dll.old-*" 2^>nul') do (
    copy /y "%OUT%\%%F" "%OUT%\zwift_vr.dll" >nul & echo Build failed - restored previous zwift_vr.dll from %%F & goto :restored
  )
)
:restored
exit /b 1
