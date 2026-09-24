@echo off
rem build.cmd [Debug|Release]  -  builds the whole solution for x64.
rem Output: build\bin\<Configuration>\
setlocal
set NoDefaultCurrentDirectoryInExePath=

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set MSBUILD=
if exist %VSWHERE% (
    for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
)
if "%MSBUILD%"=="" (
    echo MSBuild.exe could not be found. Install Visual Studio with the "Desktop development with C++" workload.
    exit /b 1
)

"%MSBUILD%" "%~dp0ShadePatcher.sln" /m /nologo /v:m /p:Configuration=%CONFIG% /p:Platform=x64
exit /b %ERRORLEVEL%
