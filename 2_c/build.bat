@echo off
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
set "MODE=%~1"

if "%MODE%"=="" (
    echo Uso:
    echo   build.bat msvc
    echo   build.bat mingw
    exit /b 1
)

if /I "%MODE%"=="msvc" goto :build_msvc
if /I "%MODE%"=="mingw" goto :build_mingw

echo Modo invalido: %MODE%
echo Use "msvc" ou "mingw".
exit /b 1

:build_msvc
where cl >nul 2>nul
if errorlevel 1 (
    echo cl nao encontrado no PATH.
    echo Abra o "x64 Native Tools Command Prompt for VS" e tente novamente.
    exit /b 1
)

pushd "%ROOT%"

call cl /nologo /EHsc gateway.c smartcity_common.c ws2_32.lib /Fe:gateway.exe
if errorlevel 1 goto :build_failed

call cl /nologo /EHsc sensor_temperature.c smartcity_common.c ws2_32.lib /Fe:sensor_temperature.exe
if errorlevel 1 goto :build_failed

call cl /nologo /EHsc sensor_air_quality.c smartcity_common.c ws2_32.lib /Fe:sensor_air_quality.exe
if errorlevel 1 goto :build_failed

call cl /nologo /EHsc actuator_camera.c smartcity_common.c ws2_32.lib /Fe:actuator_camera.exe
if errorlevel 1 goto :build_failed

call cl /nologo /EHsc actuator_traffic_light.c smartcity_common.c ws2_32.lib /Fe:actuator_traffic_light.exe
if errorlevel 1 goto :build_failed

call cl /nologo /EHsc actuator_street_light.c smartcity_common.c ws2_32.lib /Fe:actuator_street_light.exe
if errorlevel 1 goto :build_failed

call cl /nologo /EHsc client.c ws2_32.lib /Fe:client.exe
if errorlevel 1 goto :build_failed

popd
echo Build concluido com MSVC.
exit /b 0

:build_mingw
where gcc >nul 2>nul
if errorlevel 1 (
    echo gcc nao encontrado no PATH.
    echo Instale MinGW-w64 e tente novamente.
    exit /b 1
)

pushd "%ROOT%"

gcc gateway.c smartcity_common.c -lws2_32 -o gateway.exe
if errorlevel 1 goto :build_failed

gcc sensor_temperature.c smartcity_common.c -lws2_32 -o sensor_temperature.exe
if errorlevel 1 goto :build_failed

gcc sensor_air_quality.c smartcity_common.c -lws2_32 -o sensor_air_quality.exe
if errorlevel 1 goto :build_failed

gcc actuator_camera.c smartcity_common.c -lws2_32 -o actuator_camera.exe
if errorlevel 1 goto :build_failed

gcc actuator_traffic_light.c smartcity_common.c -lws2_32 -o actuator_traffic_light.exe
if errorlevel 1 goto :build_failed

gcc actuator_street_light.c smartcity_common.c -lws2_32 -o actuator_street_light.exe
if errorlevel 1 goto :build_failed

gcc client.c -lws2_32 -o client.exe
if errorlevel 1 goto :build_failed

popd
echo Build concluido com MinGW.
exit /b 0

:build_failed
set "ERR=%errorlevel%"
popd
echo Build falhou. Codigo de erro: %ERR%
exit /b %ERR%
