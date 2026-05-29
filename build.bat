@echo off
setlocal

set BASH=C:\msys64\usr\bin\bash.exe
set PROJ=/c/Users/omar/SamaraOS

if not exist "%BASH%" (
    echo [build.bat] msys64 bash not found at %BASH%
    exit /b 1
)

set TARGET=%1
if "%TARGET%"=="" set TARGET=run

if /I "%TARGET%"=="build" (
    "%BASH%" -lc "cd %PROJ% && make -j"
    goto :end
)

if /I "%TARGET%"=="clean" (
    "%BASH%" -lc "cd %PROJ% && make clean"
    goto :end
)

if /I "%TARGET%"=="doom" (
    "%BASH%" -lc "cd %PROJ% && make -j && make run-doom"
    goto :end
)

if /I "%TARGET%"=="debug" (
    "%BASH%" -lc "cd %PROJ% && make -j && make run-debug"
    goto :end
)

if /I "%TARGET%"=="run" (
    "%BASH%" -lc "cd %PROJ% && make -j && make run"
    goto :end
)

echo [build.bat] unknown target: %TARGET%
echo usage: build.bat [run^|build^|doom^|debug^|clean]
exit /b 1

:end
endlocal
exit /b %ERRORLEVEL%
