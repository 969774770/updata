@echo off
rem ============================================================
rem  构建 updata32.dll / updata64.dll（更新检测模块）
rem    updata.exe 会被压缩后内嵌进 DLL，运行时解压释放到 %TEMP%\updata_run\
rem    1) 重新编译 updata.exe
rem    2) 用 tools\pack.exe 压缩成 build\payload.bin
rem    3) 编译资源（内嵌 payload）+ 32 位 DLL（MinGW 优先）/ 64 位 DLL
rem  产物：updata32.dll、updata64.dll（与本脚本同目录）
rem  导出：updata / updata_w（__stdcall，未修饰名字，易语言可直接声明）
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set OUT32=updata32.dll
set OUT64=updata64.dll
set DLLSRC=src\dll.c src\manifest.c src\http.c src\json.c src\sha256.c src\util.c
set DLLDEF=src\dll.def
set DLLRC=src\dll.rc
set RESDIR=build\res
set SDKROOT=C:\Program Files (x86)\Windows Kits\10
rem DLL 只依赖 WinHTTP / USER32，不链接 rstrtmgr 等 exe 专有库
set MSVC_LIBS=winhttp.lib user32.lib
set MINGW_LIBS=-lwinhttp -luser32 -static -static-libgcc "-Wl,--gc-sections"
set MIN_GCC=-Os -s -shared -Wall -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -ffunction-sections -fdata-sections

if not exist %RESDIR% mkdir %RESDIR%
if not exist build\dll32 mkdir build\dll32
if not exist build\dll64 mkdir build\dll64

echo ============================================
echo   构建 updata32.dll / updata64.dll
echo ============================================
echo.

echo [0/4] 编译 updata.exe（内嵌用）...
call build.bat >nul
if errorlevel 1 (
    echo       编译 updata.exe 失败：若 updata.exe 正在运行请先关闭后重试
    exit /b 1
)
if not exist updata.exe (
    echo       未找到 updata.exe
    exit /b 1
)
echo       完成

echo [1/4] 压缩内嵌的 updata.exe ...
if not exist build\pack.exe gcc -O2 -s tools\pack.c -o build\pack.exe
if not exist build\pack.exe (
    echo       编译 tools\pack.c 失败（需要 gcc）
    exit /b 1
)
build\pack.exe updata.exe build\payload.bin
if errorlevel 1 exit /b 1

rem ---------- 探测编译器 ----------
set HAVE_MINGW32=
where gcc >nul 2>nul
if not errorlevel 1 set HAVE_MINGW32=1
set X64GCC=
for %%p in (x86_64-w64-mingw32-gcc.exe) do if not "%%~$PATH:p"=="" set X64GCC=%%~$PATH:p

set VCTOOLS=
for /f "delims=" %%d in ('dir /b /ad /o-n "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" 2^>nul') do if not defined VCTOOLS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%%d\bin\HostX64\x64\cl.exe" set "VCTOOLS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\%%d"
if not defined VCTOOLS for /d %%e in ("C:\Program Files\Microsoft Visual Studio\2022\*") do if not defined VCTOOLS for /f "delims=" %%d in ('dir /b /ad /o-n "%%e\VC\Tools\MSVC" 2^>nul') do if exist "%%e\VC\Tools\MSVC\%%d\bin\HostX64\x64\cl.exe" set "VCTOOLS=%%e\VC\Tools\MSVC\%%d"
set SDKVER=
for /f "delims=" %%d in ('dir /b /ad /o-n "%SDKROOT%\Include\10.*" 2^>nul') do if not defined SDKVER if exist "%SDKROOT%\Lib\%%d\um\x64" set "SDKVER=%%d"

if not defined VCTOOLS goto :no_msvc
if not defined SDKVER goto :no_msvc
echo       检测到 MSVC 工具集与 Windows SDK
goto :detected

:no_msvc
set VCTOOLS=
set SDKVER=

:detected
if defined X64GCC echo       检测到 64 位 MinGW
echo.

rem ============================================================
rem  32 位
rem ============================================================
if defined HAVE_MINGW32 goto :build32_mingw
if defined VCTOOLS goto :build32_msvc
echo       没有可用的 32 位编译器（需要 MinGW 或 MSVC）
exit /b 1

:build32_mingw
echo [2/4] 构建 %OUT32% ^(MinGW 32 位^) ...
windres %DLLRC% -O coff -o %RESDIR%\dll32.res --include-dir=.
if errorlevel 1 goto :fail
gcc %MIN_GCC% %DLLSRC% %DLLDEF% %RESDIR%\dll32.res -o %OUT32% %MINGW_LIBS%
if errorlevel 1 goto :fail32
goto :build64

:build32_msvc
echo [2/4] 构建 %OUT32% ^(MSVC x86 / 静态 CRT^) ...
"%SDKROOT%\bin\%SDKVER%\x64\rc.exe" /nologo /fo %RESDIR%\dll.res %DLLRC%
if errorlevel 1 goto :fail
set "INCLUDE=%VCTOOLS%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\shared"
set "LIB=%VCTOOLS%\lib\x86;%SDKROOT%\Lib\%SDKVER%\ucrt\x86;%SDKROOT%\Lib\%SDKVER%\um\x86"
"%VCTOOLS%\bin\HostX86\x86\cl.exe" /nologo /O1 /Gy /W3 /TC /std:c17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /MT /LD %DLLSRC% /Fe:%OUT32% /Fo:build\dll32\ /link /DEF:%DLLDEF% /MACHINE:X86 /OPT:REF /OPT:ICF /IMPLIB:build\dll32\updata32.lib %RESDIR%\dll.res %MSVC_LIBS%
if errorlevel 1 goto :fail32

rem ============================================================
rem  64 位
rem ============================================================
:build64
if not defined X64GCC goto :build64_msvc
echo [3/4] 构建 %OUT64% ^(MinGW 64 位^) ...
windres %DLLRC% -O coff -F pe-x86-64 -o %RESDIR%\dll64.res --include-dir=.
if errorlevel 1 goto :fail
"%X64GCC%" %MIN_GCC% %DLLSRC% %DLLDEF% %RESDIR%\dll64.res -o %OUT64% %MINGW_LIBS%
if errorlevel 1 goto :fail64
goto :summary

:build64_msvc
if not defined VCTOOLS (
    echo       未找到 64 位编译器，跳过 %OUT64%
    goto :summary
)
echo [3/4] 构建 %OUT64% ^(MSVC x64 / 静态 CRT^) ...
"%SDKROOT%\bin\%SDKVER%\x64\rc.exe" /nologo /fo %RESDIR%\dll.res %DLLRC%
if errorlevel 1 goto :fail
set "INCLUDE=%VCTOOLS%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\shared"
set "LIB=%VCTOOLS%\lib\x64;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"
"%VCTOOLS%\bin\HostX64\x64\cl.exe" /nologo /O1 /Gy /W3 /TC /std:c17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /MT /LD %DLLSRC% /Fe:%OUT64% /Fo:build\dll64\ /link /DEF:%DLLDEF% /MACHINE:X64 /OPT:REF /OPT:ICF /IMPLIB:build\dll64\updata64.lib %RESDIR%\dll.res %MSVC_LIBS%
if errorlevel 1 goto :fail64

:summary
echo.
echo 构建完成：
if exist %OUT32% for %%f in (%OUT32%) do echo    %%~nxf  %%~zf 字节
if exist %OUT64% for %%f in (%OUT64%) do echo    %%~nxf  %%~zf 字节
echo.
echo 提示：updata.exe 已压缩内嵌在 DLL 内，部署时只需要 DLL（加宿主程序）。
exit /b 0

:fail
echo.
echo DLL 资源编译失败
exit /b 1

:fail32
echo.
echo 32 位构建失败！
exit /b 1

:fail64
echo.
echo 64 位构建失败（32 位已生成，可单独使用）！
exit /b 1
