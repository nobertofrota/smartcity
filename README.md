# Smart City Sockets - Trabalho 1

Projeto de simulação de cidade inteligente com processos distribuídos usando **TCP**, **UDP**, **UDP Multicast** e **Protocol Buffers**.

## Estrutura

```text
smart_city_sockets/
├── protos/
│   └── messages.proto
├── generated/
│   ├── __init__.py
│   └── messages_pb2.py
├── gateway.py
├── client.py
├── sensor_temperature.py
├── sensor_air_quality.py
├── sensor_noise.py
├── requirements.txt
└── README.md
```

## Requisitos

- Python 3.10+
- Terminal com suporte a múltiplos processos

## Instalação

```bash
pip install -r requirements.txt
```

## Compilar Protobuf

No diretório `smart_city_sockets`:

```bash
python -m grpc_tools.protoc -I=protos --python_out=generated protos/messages.proto
```

## Execução

Abra terminais separados dentro de `smart_city_sockets/`.

1. Iniciar o Gateway:

```bash
python gateway.py
```

2. Iniciar sensores:

```bash
python sensor_temperature.py
python sensor_air_quality.py
python sensor_noise.py
```

3. Iniciar cliente analítico:

```bash
python client.py
```

## Portas e protocolos

- Gateway TCP (cliente): `9000`
- Gateway UDP (leituras): `9001`
- Multicast descoberta: `224.1.1.1:10000`
- Respostas de descoberta para gateway: `10001` (UDP)
- Sensor controlável (ar) TCP controle: `9101`

## Funcionalidades

- Descoberta automática de sensores via UDP Multicast.
- Sensores enviando leituras via UDP para o Gateway.
- Gateway centralizando sensores e histórico de leituras em memória.
- Cliente TCP com menu para:
  - listar sensores
  - média de temperatura
  - média de CO2
  - maior leitura registrada
  - ativar/desativar sensor controlável
  - alterar frequência do sensor controlável
  - alterar limiar de alerta do sensor controlável
- Sensor de qualidade do ar com alerta quando CO2 ultrapassa limiar.

## Exemplo de fluxo para vídeo (até 7 min)

1. Mostrar estrutura do projeto e `messages.proto`.
2. Executar `gateway.py` e destacar as portas.
3. Executar os três sensores e mostrar no gateway as descobertas multicast.
4. Mostrar leituras UDP chegando continuamente no gateway.
5. Executar `client.py`.
6. Opção `1`: listar sensores conectados.
7. Opção `2` e `3`: exibir médias de temperatura e CO2.
8. Opção `4`: exibir maior leitura registrada.
9. Opção `8`: alterar limiar do sensor `air-1` para valor menor (ex.: 600).
10. Mostrar alertas no gateway quando CO2 ultrapassar limiar.
11. Opção `7`: alterar frequência do `air-1` (ex.: 1.0s e depois 3.0s) e observar mudança no ritmo de envio.
12. Opção `6`: desativar `air-1` e mostrar pausa de leituras de CO2.
13. Opção `5`: reativar `air-1` e mostrar retorno do envio.
14. Opção `9`: encerrar cliente.

## Observações

- O sistema trata falhas de conexão com mensagens de erro sem travar o processo.
- Se um sensor cair, o gateway continua executando e aceitando cliente.
- O histórico é mantido apenas em memória (reiniciar gateway limpa dados).
