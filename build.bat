@echo off
setlocal

set "PATH=C:\w64devkit\bin;%PATH%"
set CXX=C:\w64devkit\bin\g++.exe
set IMGUI_PATH=imgui
set VCPKG=C:\vcpkg\installed\x64-windows

set CFLAGS=-std=c++20 -O2 -Wall -I%IMGUI_PATH% -I%IMGUI_PATH%\backends -I%VCPKG%\include
set LDFLAGS=-L%VCPKG%\lib -lglfw3dll -lopengl32 -lgdi32 -lgdiplus -lshell32 -luser32 -lkernel32 -lwinmm -ldwmapi -lurlmon -lwinhttp -lws2_32 -ladvapi32 -lshlwapi -ldiscord-rpc

echo Building AmethystLauncher...

%CXX% %CFLAGS% ^
    src\main.cpp ^
    %IMGUI_PATH%\imgui.cpp ^
    %IMGUI_PATH%\imgui_draw.cpp ^
    %IMGUI_PATH%\imgui_widgets.cpp ^
    %IMGUI_PATH%\imgui_tables.cpp ^
    %IMGUI_PATH%\backends\imgui_impl_glfw.cpp ^
    %IMGUI_PATH%\backends\imgui_impl_opengl3.cpp ^
    %LDFLAGS% ^
    -o build\AmethystLauncher.exe

if %errorlevel% neq 0 (
    echo Build failed!
    exit /b 1
)

echo Build successful!

if exist "%VCPKG%\bin\glfw3.dll" (
    copy /Y "%VCPKG%\bin\glfw3.dll" build\ >nul
)

if exist "%VCPKG%\bin\discord-rpc.dll" (
    copy /Y "%VCPKG%\bin\discord-rpc.dll" build\ >nul
)

echo Done! Run build\AmethystLauncher.exe
endlocal
