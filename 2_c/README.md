# Smart City - versao C

Este guia foi escrito para execucao manual no Windows, sem automatizacao, com foco em reduzir erro de digitação e de ordem de subida dos processos.

## O que voce vai rodar

- `gateway.exe`
- `sensor_temperature.exe`
- `actuator_traffic_light.exe`
- `client.exe`
- opcionalmente `sensor_air_quality.exe`

## Requisitos no Windows

- Windows 10 ou superior
- `MSYS2 UCRT64` com `gcc` instalado
- abrir todos os executaveis no mesmo computador
- permitir a comunicacao de rede local no firewall, se o Windows perguntar

## Instalacao do compilador

Se o `gcc` ainda nao estiver instalado, abra um terminal PowerShell como usuario normal e rode:

```powershell
winget install -e --id MSYS2.MSYS2
```

Depois abra o atalho **MSYS2 UCRT64** e rode:

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

## Compilacao

Abra o PowerShell na raiz do projeto e rode:

```powershell
cd D:\smartcity\smart_city_sockets\2_c
.\build.bat mingw
```

Se o seu ambiente for Visual Studio, tambem e possivel usar:

```powershell
cd D:\smartcity\smart_city_sockets\2_c
.\build.bat msvc
```

Se a compilacao terminar sem erro, os executaveis ficam na pasta `2_c`.

## Ordem correta de execucao

Abra um terminal separado para cada processo, nesta ordem:

```powershell
.\gateway.exe
```

```powershell
.\sensor_temperature.exe
```

```powershell
.\actuator_traffic_light.exe
```

```powershell
.\client.exe
```

Se quiser testar leitura adicional, abra depois:

```powershell
.\sensor_air_quality.exe
```

## Roteiro manual completo

Use este roteiro exatamente nesta ordem:

1. Inicie `sensor_temperature.exe`.
2. Inicie `actuator_traffic_light.exe`.
3. Inicie `gateway.exe`.
4. Aguarde cerca de 10 segundos para a descoberta inicial estabilizar.
5. Inicie `client.exe`.
6. No cliente, digite `1` para listar os dispositivos conectados.
7. Confirme que aparecem `temp-1` e `semaforo-1`.
8. No cliente, digite `9` para enviar comando ao semaforo.
9. Quando o cliente pedir o ID, digite `semaforo-1`.
10. Quando o cliente pedir a cor, digite `vermelho`.
11. No cliente, digite `6` para consultar o historico do sensor.
12. Feche o terminal do `sensor_temperature.exe`.
13. Aguarde cerca de 10 segundos.
14. No cliente, digite `1` outra vez.
15. Confirme que `temp-1` aparece como inativo.
16. Abra novamente `sensor_temperature.exe`.
17. Aguarde cerca de 10 segundos para o gateway reencontrar o sensor.
18. No cliente, digite `1` novamente.
19. Confirme que `temp-1` voltou a ficar ativo.
20. Feche o terminal do `gateway.exe`.
21. Tente usar o cliente novamente e confirme que a conexao falha.
22. Abra `gateway.exe` de novo.
23. Aguarde cerca de 10 segundos.
24. Reabra `client.exe`.
25. No cliente, digite `1` para reconectar e listar os dispositivos.
26. Se quiser testar outro comando, use `7`, `8`, `9` ou `10` e informe o ID solicitado.
27. Se quiser testar leitura adicional, abra `sensor_air_quality.exe` e use `1` ou `5`.
28. Para encerrar, feche primeiro o cliente.
29. Depois feche os sensores.
30. Por ultimo, feche o gateway.

## O que digitar no cliente

- `1` lista os dispositivos
- `2` mostra detalhes
- `3` mostra o ultimo estado conhecido
- `4` mostra o menor valor
- `5` mostra o maior valor
- `6` mostra o historico
- `7` envia comando para ligar/desligar
- `8` consulta um estado especifico
- `9` altera a cor do semaforo
- `10` mostra dados do sensor
- `11` encerra o cliente

## IDs usados no teste

- `temp-1` para o sensor de temperatura
- `semaforo-1` para o semaforo
- `air-1` para o sensor de qualidade do ar

## Se algo nao responder

- Se o cliente nao conectar logo depois que o gateway abrir, aguarde 10 segundos e tente de novo.
- Se um sensor acabou de ser reiniciado e ainda nao aparece na lista, aguarde 10 segundos antes de consultar.
- Se um comando retornar `Dispositivo nao encontrado`, confira se o ID foi digitado exatamente como `temp-1`, `semaforo-1` ou `air-1`.
- Se o Windows bloquear porta ou firewall, permita a execucao dos executaveis locais.

## Observacao

Este fluxo foi escrito para ser executado manualmente no Windows, sem depender de script de teste.
