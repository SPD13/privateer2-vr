@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist ..\build\stub mkdir ..\build\stub
set OXR=..\ref\openxr
cl /nologo /O2 /LD /MT p2den_stub.cpp /Fe..\build\stub\p2den.dll /Fo..\build\ /link /DEF:p2den_stub.def /IMPLIB:..\build\p2den.lib >nul || exit /b 1
cl /nologo /O2 /LD /MT /W3 /EHsc /I%OXR%\include p2vr.cpp vr.cpp capture.cpp macro.cpp smooth.cpp /Fe..\build\p2vr.dll /Fo..\build\ /link /DEF:p2vr.def /LIBPATH:%OXR%\Win32\lib ..\build\p2den.lib user32.lib gdi32.lib winmm.lib || exit /b 1
copy /y %OXR%\Win32\bin\openxr_loader.dll ..\build\ >nul
echo BUILD OK
