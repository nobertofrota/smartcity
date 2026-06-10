# Reimplementacao em Python

Esta pasta contem a implementacao original dos processos do trabalho em Python.

## Requisitos

Voce precisa de:

- Python 3.11 ou superior
- `pip`
- macOS, Linux ou Windows

## Instalacao

Crie um ambiente virtual e instale as dependencias:

```bash
cd /caminho/para/smart_city_sockets
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
```

No Windows PowerShell:

```powershell
cd D:\smartcity\smart_city_sockets
python -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

## Como executar

Use sempre a mesma pasta `1_python`. Para o roteiro manual, siga esta ordem:

1. `gateway.py`
2. `sensor_temperature.py`
3. `actuator_traffic_light.py`
4. `client.py`
5. `sensor_air_quality.py` opcional

### Comandos

#### macOS e Linux

```bash
cd /caminho/para/smart_city_sockets/1_python
python3 gateway.py
```

```bash
cd /caminho/para/smart_city_sockets/1_python
python3 sensor_temperature.py
```

```bash
cd /caminho/para/smart_city_sockets/1_python
python3 actuator_traffic_light.py
```

```bash
cd /caminho/para/smart_city_sockets/1_python
python3 client.py
```

Opcional:

```bash
cd /caminho/para/smart_city_sockets/1_python
python3 sensor_air_quality.py
```

Opcional, painel web:

```bash
cd /caminho/para/smart_city_sockets/1_python
python3 -m streamlit run web_streamlit.py
```

#### Windows PowerShell

```powershell
cd D:\smartcity\smart_city_sockets\1_python
python gateway.py
```

```powershell
cd D:\smartcity\smart_city_sockets\1_python
python sensor_temperature.py
```

```powershell
cd D:\smartcity\smart_city_sockets\1_python
python actuator_traffic_light.py
```

```powershell
cd D:\smartcity\smart_city_sockets\1_python
python client.py
```

Opcional:

```powershell
cd D:\smartcity\smart_city_sockets\1_python
python sensor_air_quality.py
```

Opcional, painel web:

```powershell
cd D:\smartcity\smart_city_sockets\1_python
python -m streamlit run web_streamlit.py
```

## Roteiro Manual

1. Abra o `gateway.py`.
2. Abra o `sensor_temperature.py`.
3. Abra o `actuator_traffic_light.py`.
4. Abra o `client.py`.
5. No cliente, digite `1` para listar dispositivos.
6. Confirme que aparecem `temp-1` e `semaforo-1`.
7. Digite `9` para alterar a cor do semaforo.
8. Informe `semaforo-1` como ID e `vermelho` como cor.
9. Digite `6` para ver o historico de leituras.
10. Feche o terminal do `sensor_temperature.py`.
11. Aguarde cerca de 10 segundos.
12. Digite `1` no cliente novamente e confirme que `temp-1` ficou inativo.
13. Reabra `sensor_temperature.py` no mesmo diretório.
14. Aguarde cerca de 10 segundos para o gateway descobrir o sensor novamente.
15. Digite `1` no cliente e confirme que `temp-1` voltou a ficar ativo.
16. Feche o terminal do `gateway.py`.
17. Tente usar o cliente novamente e confirme a falha de conexao.
18. Reabra o `gateway.py`.
19. Aguarde cerca de 10 segundos para a descoberta estabilizar.
20. Abra o cliente de novo e repita `1` para reconectar e listar os dispositivos.
21. Digite `11` para encerrar o cliente.

## Observacoes

- A interface web agora usa deteccao de processos multiplataforma com `psutil`.
- O schema protobuf ja esta gerado em `generated/messages_pb2.py`.
- O gateway grava cada leitura recebida em `data/csv/<sensor_id>.csv`.
- O cliente web e o cliente de console usam o mesmo protocolo TCP com framing de 4 bytes.
- No roteiro manual, o comando do semaforo usa o ID `semaforo-1`.
- O sensor de temperatura usa o ID `temp-1`.
- O sensor de qualidade do ar usa o ID `air-1`.
