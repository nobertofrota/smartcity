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

## Build

Os exemplos abaixo usam MSVC no Windows.

Para compilar tudo de uma vez:

```bat
build.bat msvc
```

Ou, se estiver usando MinGW:

```bat
build.bat mingw
```

Compile cada executavel separadamente:

```bat
cl /EHsc gateway.c smartcity_common.c ws2_32.lib /Fe:gateway.exe
cl /EHsc sensor_temperature.c smartcity_common.c ws2_32.lib /Fe:sensor_temperature.exe
cl /EHsc sensor_air_quality.c smartcity_common.c ws2_32.lib /Fe:sensor_air_quality.exe
cl /EHsc actuator_camera.c smartcity_common.c ws2_32.lib /Fe:actuator_camera.exe
cl /EHsc actuator_traffic_light.c smartcity_common.c ws2_32.lib /Fe:actuator_traffic_light.exe
cl /EHsc actuator_street_light.c smartcity_common.c ws2_32.lib /Fe:actuator_street_light.exe
cl /EHsc client.c ws2_32.lib /Fe:client.exe
```

## Execucao

Ordem recomendada:

1. `gateway.exe`
2. `sensor_temperature.exe`
3. `sensor_air_quality.exe`
4. `actuator_camera.exe`
5. `actuator_traffic_light.exe`
6. `actuator_street_light.exe`
7. `client.exe`

## Observacoes

- O cliente web em Streamlit nao foi portado para C, porque ele era uma interface opcional.
- A comunicacao principal do trabalho esta coberta pelos processos acima.
- O gateway grava cada leitura recebida em `data/csv/<sensor_id>.csv`.
- O cliente analitico tambem pode consultar o historico de leituras retornado pelo gateway.
- Os logs da execucao automatizada ficam em `../1_python/runtime_logs/` quando o cliente Web em Python e usado para iniciar os processos.
