@echo off
chcp 65001 >nul
rem tree_bench 빌드 (Windows). 결과: bin\tree_bench.exe
rem trees\ 와 trees\<이름>\ 아래 .cc 를 모두 같이 컴파일하므로, 새 방식을 추가해도 이 파일은 고치지 않는다.
setlocal enabledelayedexpansion
cd /d "%~dp0.."

rem WinLibs g++ (winget 설치)가 있으면 PATH 맨 앞에 둔다. 옛 MinGW(6.x)가 먼저 잡히는 문제 방지
for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT*") do (
  if exist "%%d\mingw64\bin\g++.exe" set "PATH=%%d\mingw64\bin;!PATH!"
)
set GV=
for /f "delims=." %%v in ('g++ -dumpversion 2^>nul') do set GV=%%v
if not defined GV (
  echo [ERROR] g++ not found. Install: winget install BrechtSanders.WinLibs.POSIX.UCRT  ^(then open a new cmd window^)
  exit /b 1
)
if !GV! LSS 13 (
  echo [ERROR] g++ version !GV! found, need 13+. Install: winget install BrechtSanders.WinLibs.POSIX.UCRT  ^(then open a new cmd window^)
  exit /b 1
)

if not exist bin mkdir bin
set SRCS=bench\tree_bench.cpp third_party\arctic\suffix_tree.cc
for %%f in (trees\*.cc) do set SRCS=!SRCS! %%f
for /d %%d in (trees\*) do for %%f in (%%d\*.cc) do set SRCS=!SRCS! %%f
echo g++ !SRCS!
g++ -O2 -DNDEBUG -std=c++20 -static -pthread !SRCS! -o bin\tree_bench.exe
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo built bin\tree_bench.exe
bin\tree_bench.exe --list
