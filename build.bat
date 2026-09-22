@echo off
rem ============================================================
rem  构建 updata.exe（差异化更新程序，Win32 GUI）
rem  依赖: gcc / windres (MinGW-W64)
rem  产物: updata.exe
rem ============================================================
setlocal
cd /d "%~dp0"

set CC=gcc
set RC=windres
set OUT=updata.exe
set CFLAGS=-Os -s -mwindows -Wall -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -ffunction-sections -fdata-sections
set SRCS=src\main.c src\update.c src\manifest.c src\http.c src\json.c src\sha256.c src\util.c src\lockcheck.c
set LIBS=-lwinhttp -lrstrtmgr -lcomctl32 -lgdi32 -luser32 -lole32 -lshell32 -static -static-libgcc -Wl,--gc-sections

if not exist build mkdir build
if not exist build\res mkdir build\res

rem MinGW 链接时会自动加入自带的 default-manifest.o（清单资源 ID 冲突），
rem 这里放一个空的同名目标文件并用 -B 优先搜索，以便使用本程序自己的清单。
if not exist build\empty mkdir build\empty
if not exist build\empty\t.c echo. > build\empty\t.c
%CC% -c build\empty\t.c -o build\empty\default-manifest.o
if errorlevel 1 goto :fail

echo [1/2] 编译资源...
%RC% src\app.rc -O coff -o build\res\app.res --include-dir=src --include-dir=.
if errorlevel 1 goto :fail

echo [2/2] 编译链接...
%CC% %CFLAGS% -B build\empty\ %SRCS% build\res\app.res -o %OUT% %LIBS%
if errorlevel 1 goto :fail

echo.
echo 构建成功: %OUT%
for %%f in (%OUT%) do echo   大小: %%~zf 字节
exit /b 0

:fail
echo.
echo 构建失败!
exit /b 1
