
# Link do vídeo apresentando o projeto:
https://youtu.be/5yT9DnghPhc

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
├── actuator_camera.py
├── actuator_traffic_light.py
├── actuator_street_light.py
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
```

3. Iniciar atuadores controláveis:

```bash
python actuator_camera.py
python actuator_traffic_light.py
python actuator_street_light.py
```

4. Iniciar cliente analítico:

```bash
python client.py
```

5. Iniciar cliente web Streamlit:

```bash
streamlit run web_streamlit.py
```

## Portas e protocolos

- Gateway TCP (cliente): `9000`
- Gateway UDP (leituras): `9001`
- Multicast descoberta: `224.1.1.1:10000`
- Respostas de descoberta para gateway: `10001` (UDP)
- Câmera TCP controle: `9201`
- Semáforo TCP controle: `9202`
- Luz do poste TCP controle: `9203`

## Funcionalidades

- Descoberta automática de sensores e atuadores via UDP Multicast.
- Sensores descobrem o IP e a porta UDP do Gateway a partir da mensagem multicast.
- Sensores enviando leituras via UDP para o Gateway.
- Sensor de temperatura enviando temperatura.
- Sensor de qualidade do ar enviando CO2 e umidade.
- Atuadores controláveis via TCP: câmera, semáforo e luz do poste.
- Gateway centralizando sensores e histórico de leituras em memória.
- Gateway marca dispositivos como inativos quando ficam sem discovery/leitura recente.
- Cliente TCP com menu para:
  - listar dispositivos
  - média de temperatura
  - média de CO2
  - média de umidade
  - maior leitura registrada
  - enviar comandos para atuadores
- Cliente Web Streamlit para listar sensores/atuadores e controlar câmera, semáforo e poste pelo Gateway.
- Painel Streamlit para iniciar/desligar Gateway, sensores e atuadores durante a demonstração.
- Console de logs no Streamlit para mostrar discovery multicast, leituras UDP, conexões TCP e comandos enviados.
- Sensor de qualidade do ar com alerta quando CO2 ultrapassa limiar interno.

## Roteiro pela interface web

Depois de abrir `streamlit run web_streamlit.py`, use a aba **Processos** para iniciar e desligar o Gateway, os sensores e os atuadores.
Use a aba **Cliente analitico** para verificar a descoberta, consultar estados, monitorar métricas/gráficos e enviar comandos para os atuadores.
Use a aba **Console de logs** para demonstrar o que acontece internamente. Por padrão ela mostra apenas os eventos mais importantes para o vídeo: descoberta, leituras UDP, comandos TCP e falhas/reconexões. Marque **Mostrar logs detalhados** apenas se precisar depurar.

## Exemplo de fluxo para vídeo (até 7 min)

1. Em **Processos**, iniciar dois dispositivos: um atuador controlável (ex.: semáforo) e um sensor contínuo (ex.: temperatura).
2. Em **Processos**, iniciar o **Gateway Inteligente**.
3. Em **Cliente analitico**, mostrar se a descoberta está funcionando pela tabela de dispositivos.
4. Mostrar que o Cliente conectou ao Gateway pela mensagem **Conectado ao Gateway**.
5. Consultar os estados dos dispositivos conectados.
6. Enviar comandos para dispositivos específicos, como alterar a cor do semáforo ou ligar/desligar o poste.
7. Monitorar dados de um sensor contínuo nas métricas e no gráfico.
8. Em **Processos**, desligar o processo de algum dispositivo.
9. Em **Cliente analitico**, aguardar o dispositivo aparecer inativo.
10. Em **Processos**, iniciar novamente o processo de um dispositivo.
11. Em **Processos**, desligar o Gateway.
12. Em **Cliente analitico**, avaliar o que acontece com o Cliente e dispositivos: o cliente fica aberto, mas mostra erro de conexão.
13. Em **Processos**, reiniciar o Gateway.
14. Em **Cliente analitico**, reconectar/aguardar auto-refresh e mostrar a redescoberta.
15. Avaliar o retorno dos estados e das leituras.
16. Em **Processos**, iniciar outro dispositivo, como câmera ou luz do poste.
17. Consultar novamente os estados dos dispositivos conectados.
18. Enviar novos comandos para dispositivos específicos.
19. Em **Processos**, clicar em **Desligar sensores e atuadores**.
20. Consultar os estados dos dispositivos após o desligamento.
21. Desligar o Gateway e depois o Cliente Web.

Durante o roteiro, use **Console de logs** para provar discovery multicast, leituras UDP, conexões TCP e comandos enviados.

## Observações

- O sistema trata falhas de conexão com mensagens de erro sem travar o processo.
- Se um sensor cair, o gateway continua executando e aceitando cliente.
- O histórico é mantido apenas em memória (reiniciar gateway limpa dados).
