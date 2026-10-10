@echo off
rem Build the Taipan runner (taipan.exe) and language server (taipan-lsp.exe) on Windows.
rem   build.bat          use MSVC (cl.exe) if available, otherwise g++ (MinGW)
rem   build.bat gcc      force g++
rem Run from a "Developer Command Prompt" for MSVC, or with MinGW g++ on PATH.
setlocal
if not exist build mkdir build

if /i "%1"=="gcc" goto gcc
where cl >nul 2>nul
if errorlevel 1 goto gcc

echo Building with MSVC...
if not exist build\obj mkdir build\obj
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /Iinclude src\*.cpp tools\taipan_main.cpp /Fobuild\obj\ /Fe:build\taipan.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /Iinclude /Isrc src\*.cpp tools\taipan_lsp.cpp /Fobuild\obj\ /Fe:build\taipan-lsp.exe
if errorlevel 1 exit /b 1
echo built build\taipan.exe and build\taipan-lsp.exe
exit /b 0

:gcc
where g++ >nul 2>nul
if errorlevel 1 (
    echo No C++ compiler found. Install Visual Studio Build Tools or MinGW-w64.
    exit /b 1
)
echo Building with g++...
g++ -std=c++17 -O2 -Iinclude src\*.cpp tools\taipan_main.cpp -o build\taipan.exe
if errorlevel 1 exit /b 1
g++ -std=c++17 -O2 -Iinclude -Isrc src\*.cpp tools\taipan_lsp.cpp -o build\taipan-lsp.exe
if errorlevel 1 exit /b 1
echo built build\taipan.exe and build\taipan-lsp.exe
