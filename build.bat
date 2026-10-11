@echo off
setlocal
set MINGW=D:\mingw64\bin
set TARGET=HardwareMonitor.exe

echo [1/3] make icon
if exist "D:\My_programs\.venv\Scripts\python.exe" (
  "D:\My_programs\.venv\Scripts\python.exe" tools\make_icon.py
)

echo [2/3] windres app.rc  -^>  app_res.o
"%MINGW%\windres.exe" -c 65001 app.rc -O coff -o app_res.o
if errorlevel 1 goto fail

echo [3/3] g++ main.cpp  -^>  %TARGET%
"%MINGW%\g++.exe" -std=c++17 -O2 -municode -mwindows -finput-charset=UTF-8 -static-libgcc -static-libstdc++ -o %TARGET% main.cpp app_res.o -lpdh -liphlpapi -ldxgi -lgdi32 -luser32 -ladvapi32 -lole32 -loleaut32 -luuid -lpsapi -lpowrprof
if errorlevel 1 goto fail

echo.
echo Build OK : %TARGET%
echo.
exit /b 0

:fail
echo.
echo BUILD FAILED
echo.
exit /b 1
