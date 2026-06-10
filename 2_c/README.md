# Reimplementacao em C

Esta pasta contem a reimplementacao dos processos do trabalho em C.

## Arquitetura

- `gateway.c`
- `sensor_temperature.c`
- `sensor_air_quality.c`
- `actuator_camera.c`
- `actuator_traffic_light.c`
- `actuator_street_light.c`
- `client.c`
- `smartcity_common.c` / `smartcity_common.h`

## O que foi reaproveitado do cliente Python

- o schema protobuf de [`python/protos/messages.proto`](../python/protos/messages.proto)
- o framing TCP de 4 bytes com tamanho em big-endian
- o menu e os fluxos do cliente analitico, incluindo consulta de historico
- o modelo de discovery multicast e de controle via gateway

## Requisitos

Este projeto em C roda em Windows.

Voce precisa de:

- `PowerShell` ou `Windows Terminal`
- `MSYS2 UCRT64` com `gcc`

Se o computador ainda nao tiver a toolchain, instale com:

```powershell
winget install --id MSYS2.MSYS2 -e --scope user
```

Depois instale o compilador:

```powershell
C:\msys64\usr\bin\bash.exe -lc "yes | pacman -Sy --noconfirm --needed mingw-w64-ucrt-x86_64-gcc"
```

## Build

Abra um terminal em `2_c` e compile tudo com:

```powershell
C:\msys64\usr\bin\bash.exe -lc "export PATH=/ucrt64/bin:$PATH; cd /d/smartcity/smart_city_sockets/2_c; gcc gateway.c smartcity_common.c -lws2_32 -o gateway.exe; gcc sensor_temperature.c smartcity_common.c -lws2_32 -o sensor_temperature.exe; gcc sensor_air_quality.c smartcity_common.c -lws2_32 -o sensor_air_quality.exe; gcc actuator_camera.c smartcity_common.c -lws2_32 -o actuator_camera.exe; gcc actuator_traffic_light.c smartcity_common.c -lws2_32 -o actuator_traffic_light.exe; gcc actuator_street_light.c smartcity_common.c -lws2_32 -o actuator_street_light.exe; gcc client.c -lws2_32 -o client.exe"
```

Se preferir MSVC, `build.bat` tambem existe:

```bat
build.bat msvc
```

## Como executar

Todos os processos devem ser iniciados em terminais separados na pasta `2_c`.

### Ordem recomendada

1. `gateway.exe`
2. `sensor_temperature.exe`
3. `sensor_air_quality.exe`
4. `actuator_camera.exe`
5. `actuator_traffic_light.exe`
6. `actuator_street_light.exe`
7. `client.exe`

### Comandos

```powershell
cd D:\smartcity\smart_city_sockets\2_c
.\gateway.exe
```

Em outro terminal:

```powershell
cd D:\smartcity\smart_city_sockets\2_c
.\sensor_temperature.exe
```

```powershell
cd D:\smartcity\smart_city_sockets\2_c
.\sensor_air_quality.exe
```

```powershell
cd D:\smartcity\smart_city_sockets\2_c
.\actuator_camera.exe
```

```powershell
cd D:\smartcity\smart_city_sockets\2_c
.\actuator_traffic_light.exe
```

```powershell
cd D:\smartcity\smart_city_sockets\2_c
.\actuator_street_light.exe
```

```powershell
cd D:\smartcity\smart_city_sockets\2_c
.\client.exe
```

## Fluxo minimo de teste

Depois que todos os processos estiverem rodando:

1. Abra o `client.exe`
2. Escolha `1` para listar os dispositivos
3. Escolha `6` para ver o historico de leituras
4. Escolha `7`, `8`, `9` ou `10` para enviar comandos aos atuadores
5. Escolha `11` para sair do cliente

## Observacoes

- O cliente web em Streamlit nao foi portado para C, porque ele era uma interface opcional.
- A comunicacao principal do trabalho esta coberta pelos processos acima.
- O gateway grava cada leitura recebida em `data/csv/<sensor_id>.csv`.
- O cliente analitico tambem pode consultar o historico de leituras retornado pelo gateway.
- Se o gateway for desligado, o cliente vai falhar na conexao ate que ele volte a subir.
