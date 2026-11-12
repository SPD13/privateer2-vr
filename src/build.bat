@echo off
rem Builds build\p2vr.dll (x86) and copies openxr_loader.dll next to it.
rem   P2VR_VERSION  version compiled into the DLL (default: dev); the release workflow sets it from the tag
rem   OXR           OpenXR SDK folder with include\ and Win32\lib, Win32\bin (default: ..\ref\openxr)
rem   P2VR_BHAPTICS_KEY, P2VR_BHAPTICS_WORKSPACE  bHaptics SDK API key and workspace ID compiled in (release builds: GitHub
rem                 secrets; empty otherwise, then BHAPTICS_API_KEY / BHAPTICS_WORKSPACE_ID in p2vr.ini are used)
rem Uses the x86 compiler already in PATH (e.g. GitHub Actions' msvc-dev-cmd), otherwise the newest Visual Studio's vcvars32.
setlocal
where cl >nul 2>nul || call :vcvars || exit /b 1
cd /d "%~dp0"
if not exist ..\build\stub mkdir ..\build\stub
if "%OXR%"=="" set OXR=..\ref\openxr
if "%P2VR_VERSION%"=="" set P2VR_VERSION=dev
cl /nologo /O2 /LD /MT p2den_stub.cpp /Fe..\build\stub\p2den.dll /Fo..\build\ /link /DEF:p2den_stub.def /IMPLIB:..\build\p2den.lib >nul || exit /b 1
cl /nologo /O2 /LD /MT /W3 /EHsc /I%OXR%\include /DP2VR_VERSION=\"%P2VR_VERSION%\" /DP2VR_BHAPTICS_KEY=\"%P2VR_BHAPTICS_KEY%\" /DP2VR_BHAPTICS_WORKSPACE=\"%P2VR_BHAPTICS_WORKSPACE%\" p2vr.cpp vr.cpp capture.cpp macro.cpp smooth.cpp haptics.cpp /Fe..\build\p2vr.dll /Fo..\build\ /link /DEF:p2vr.def /LIBPATH:%OXR%\Win32\lib ..\build\p2den.lib user32.lib gdi32.lib winmm.lib advapi32.lib || exit /b 1
copy /y %OXR%\Win32\bin\openxr_loader.dll ..\build\ >nul
echo BUILD OK (%P2VR_VERSION%)
exit /b 0

:vcvars
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -property installationPath`) do (
    call "%%i\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul && exit /b 0
)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
