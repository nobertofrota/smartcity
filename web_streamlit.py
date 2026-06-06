import os
import socket
import struct
import subprocess
import threading
from datetime import datetime
from pathlib import Path
import sys

import altair as alt
import pandas as pd
import streamlit as st
from streamlit_autorefresh import st_autorefresh

BASE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE_DIR))
sys.path.insert(0, str(BASE_DIR / "generated"))

from generated import messages_pb2

DEFAULT_GATEWAY_IP = "127.0.0.1"
DEFAULT_GATEWAY_PORT = 9000
TCP_TIMEOUT_SECONDS = 5
AUTO_REFRESH_INTERVAL_MS = 3000
LOG_DIR = BASE_DIR / "runtime_logs"
MAX_CLIENT_LOG_LINES = 300
MAX_CONSOLE_LINES = 500
MAX_VIDEO_CONSOLE_LINES = 120
MAX_VIDEO_READING_LINES = 12
MAX_VIDEO_DISCOVERY_LINES = 12

PROCESS_SPECS = {
    "gateway": {
        "label": "Gateway Inteligente",
        "script": "gateway.py",
        "kind": "gateway",
    },
    "sensor_air_quality": {
        "label": "Sensor de qualidade do ar (CO2/umidade)",
        "script": "sensor_air_quality.py",
        "kind": "sensor",
    },
    "sensor_temperature": {
        "label": "Sensor de temperatura (continuo)",
        "script": "sensor_temperature.py",
        "kind": "sensor",
    },
    "actuator_camera": {
        "label": "Camera",
        "script": "actuator_camera.py",
        "kind": "actuator",
    },
    "actuator_traffic_light": {
        "label": "Semaforo",
        "script": "actuator_traffic_light.py",
        "kind": "actuator",
    },
    "actuator_street_light": {
        "label": "Luz do poste",
        "script": "actuator_street_light.py",
        "kind": "actuator",
    },
}

DEVICE_TYPE_LABELS = {
    messages_pb2.TEMPERATURE_SENSOR: "Temperatura",
    messages_pb2.AIR_QUALITY_SENSOR: "Qualidade do ar",
    messages_pb2.CAMERA: "Camera",
    messages_pb2.TRAFFIC_LIGHT: "Semaforo",
    messages_pb2.STREET_LIGHT: "Luz do poste",
    messages_pb2.NOISE_SENSOR: "Ruido",
}

DEVICE_KIND_LABELS = {
    messages_pb2.SENSOR: "Sensor",
    messages_pb2.ACTUATOR: "Atuador",
}


def ensure_process_state():
    if "managed_processes" not in st.session_state:
        st.session_state.managed_processes = {}
    if "managed_log_files" not in st.session_state:
        st.session_state.managed_log_files = {}
    if "client_logs" not in st.session_state:
        st.session_state.client_logs = []
    if "last_gateway_error" not in st.session_state:
        st.session_state.last_gateway_error = None
    if "last_message" not in st.session_state:
        st.session_state.last_message = None


def add_client_log(message):
    timestamp = datetime.now().strftime("%H:%M:%S")
    st.session_state.client_logs.append(f"[{timestamp}] [Cliente Web] {message}")
    st.session_state.client_logs = st.session_state.client_logs[-MAX_CLIENT_LOG_LINES:]


def process_is_running(process):
    return process is not None and process.poll() is None


def find_script_process_ids(script_name):
    if os.name != "nt":
        return []

    ps_script = (
        "Get-CimInstance Win32_Process | "
        f"Where-Object {{ $_.Name -like 'python*' -and $_.CommandLine -like '*{script_name}*' }} | "
        "Select-Object -ExpandProperty ProcessId"
    )
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-Command", ps_script],
            capture_output=True,
            text=True,
            timeout=5,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except Exception:
        return []

    pids = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.isdigit() and int(line) != os.getpid():
            pids.append(int(line))
    return pids


def process_key_is_running(process_key):
    process = st.session_state.managed_processes.get(process_key)
    if process_is_running(process):
        return True
    return bool(find_script_process_ids(PROCESS_SPECS[process_key]["script"]))


def stop_script_processes(script_name):
    stopped = 0
    for pid in find_script_process_ids(script_name):
        try:
            subprocess.run(
                ["taskkill", "/PID", str(pid), "/F", "/T"],
                capture_output=True,
                text=True,
                timeout=5,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            stopped += 1
        except Exception:
            pass
    return stopped


def start_process(process_key):
    spec = PROCESS_SPECS[process_key]
    current = st.session_state.managed_processes.get(process_key)
    if process_is_running(current):
        st.session_state.last_message = ("success", f"{spec['label']} ja esta em execucao.")
        add_client_log(f"{spec['label']} ja estava em execucao.")
        return

    script_path = BASE_DIR / spec["script"]
    LOG_DIR.mkdir(exist_ok=True)
    log_path = LOG_DIR / f"{process_key}.log"
    log_file = open(log_path, "a", encoding="utf-8")
    log_file.write(f"\n===== {datetime.now().isoformat(timespec='seconds')} iniciando {spec['label']} =====\n")
    log_file.flush()
    process = subprocess.Popen(
        [sys.executable, "-u", str(script_path)],
        cwd=BASE_DIR,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    st.session_state.managed_processes[process_key] = process
    st.session_state.managed_log_files[process_key] = str(log_path)
    st.session_state.last_message = ("success", f"{spec['label']} iniciado.")
    add_client_log(f"Iniciado {spec['label']} (PID {process.pid}). Logs em {log_path.name}.")


def stop_process(process_key):
    spec = PROCESS_SPECS[process_key]
    process = st.session_state.managed_processes.get(process_key)
    stopped = 0

    if process_is_running(process):
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
        stopped += 1

    stopped += stop_script_processes(spec["script"])

    if stopped == 0:
        st.session_state.last_message = ("error", f"{spec['label']} nao esta em execucao.")
        add_client_log(f"Tentativa de desligar {spec['label']}, mas nenhum processo foi encontrado.")
        return

    st.session_state.last_message = ("success", f"{spec['label']} desligado ({stopped} processo(s)).")
    add_client_log(f"Desligado {spec['label']} ({stopped} processo(s)).")


def stop_all_devices():
    for key, spec in PROCESS_SPECS.items():
        if spec["kind"] in {"sensor", "actuator"}:
            process = st.session_state.managed_processes.get(key)
            if process_is_running(process):
                process.terminate()
    for key, spec in PROCESS_SPECS.items():
        if spec["kind"] in {"sensor", "actuator"}:
            process = st.session_state.managed_processes.get(key)
            if process_is_running(process):
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
            stop_script_processes(spec["script"])
    st.session_state.last_message = ("success", "Todos os sensores e atuadores encontrados foram desligados.")
    add_client_log("Desligados sensores e atuadores encontrados pelo nome do script.")


def shutdown_client():
    st.session_state.auto_refresh_enabled = False
    st.session_state.last_message = ("success", "Cliente web sera encerrado em instantes.")
    add_client_log("Cliente web solicitado para encerramento.")
    threading.Timer(1.0, lambda: os._exit(0)).start()


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def recv_msg(conn):
    header = recv_exact(conn, 4)
    if not header:
        return None
    size = struct.unpack("!I", header)[0]
    return recv_exact(conn, size)


def send_msg(conn, payload):
    conn.sendall(struct.pack("!I", len(payload)) + payload)


def send_gateway_request(host, port, req):
    try:
        is_control = req.request_type == messages_pb2.SEND_CONTROL_COMMAND
        if is_control:
            add_client_log(
                f"TCP comando -> Gateway target={req.target_sensor_id} "
                f"command={req.command_type} value={req.value} text={req.text_value or '-'}"
            )
        with socket.create_connection((host, port), timeout=TCP_TIMEOUT_SECONDS) as conn:
            send_msg(conn, req.SerializeToString())
            raw = recv_msg(conn)
            if not raw:
                add_client_log("TCP <- Gateway sem resposta.")
                return None, "Gateway encerrou a conexao sem resposta"

            resp = messages_pb2.ClientResponse()
            resp.ParseFromString(raw)
            status = "OK" if resp.status == messages_pb2.OK else "ERROR"
            if st.session_state.get("last_gateway_error"):
                add_client_log("Conexao com Gateway restabelecida.")
                st.session_state.last_gateway_error = None
            if is_control or resp.status != messages_pb2.OK:
                add_client_log(f"TCP resposta <- Gateway status={status} message='{resp.message}'")
            return resp, None
    except OSError as exc:
        error_text = str(exc)
        if st.session_state.get("last_gateway_error") != error_text:
            add_client_log(f"Falha TCP com Gateway: {exc}")
            st.session_state.last_gateway_error = error_text
        return None, f"Falha ao conectar no Gateway: {exc}"
    except Exception as exc:
        add_client_log(f"Erro inesperado ao comunicar com Gateway: {exc}")
        return None, f"Erro ao comunicar com Gateway: {exc}"


def send_simple_request(host, port, request_type):
    req = messages_pb2.ClientRequest(request_type=request_type)
    return send_gateway_request(host, port, req)


def list_sensors(host, port):
    return send_simple_request(host, port, messages_pb2.LIST_SENSORS)


def send_sensor_command(host, port, request_type, sensor_id, value=0.0):
    req = messages_pb2.ClientRequest(
        request_type=request_type,
        target_sensor_id=sensor_id,
        value=value,
    )
    return send_gateway_request(host, port, req)


def send_control_command(host, port, sensor_id, command_type, value=0.0, text_value=""):
    req = messages_pb2.ClientRequest(
        request_type=messages_pb2.SEND_CONTROL_COMMAND,
        target_sensor_id=sensor_id,
        command_type=command_type,
        value=value,
        text_value=text_value,
    )
    return send_gateway_request(host, port, req)


def sensor_to_dict(sensor):
    return {
        "ID": sensor.sensor_id,
        "Categoria": DEVICE_KIND_LABELS.get(sensor.device_kind, "Desconhecido"),
        "Tipo": DEVICE_TYPE_LABELS.get(sensor.sensor_type, f"Tipo {sensor.sensor_type}"),
        "IP": sensor.sensor_ip,
        "Porta controle": sensor.control_tcp_port,
        "Estado": "Ativo" if sensor.is_active else "Inativo",
        "Configuracao": sensor.state_text,
    }


def show_last_message():
    message = st.session_state.get("last_message")
    if not message:
        return

    kind, text = message
    if kind == "success":
        st.success(text)
    else:
        st.error(text)


def handle_command_result(resp, error):
    if error:
        st.session_state.last_message = ("error", error)
        add_client_log(f"Comando falhou antes da resposta: {error}")
        return

    if resp.status == messages_pb2.OK:
        st.session_state.last_message = ("success", resp.message)
        add_client_log(f"Comando aplicado: {resp.message}")
    else:
        st.session_state.last_message = ("error", resp.message)
        add_client_log(f"Comando recusado: {resp.message}")


def render_process_panel():
    st.subheader("Processos do roteiro")
    st.caption("Use estes botoes para gravar o video sem abrir terminais extras.")

    for key, spec in PROCESS_SPECS.items():
        process = st.session_state.managed_processes.get(key)
        running = process_key_is_running(key)

        with st.container(border=True):
            cols = st.columns([2, 1, 1, 1])
            cols[0].write(spec["label"])
            cols[1].write("Rodando" if running else "Parado")
            if running and process is not None:
                cols[2].write(f"PID {process.pid}")
            else:
                cols[2].write("-")

            if cols[3].button("Desligar" if running else "Iniciar", key=f"toggle-{key}"):
                if running:
                    stop_process(key)
                else:
                    start_process(key)
                st.rerun()

    action_cols = st.columns(3)
    if action_cols[0].button("Iniciar demo minima", width="stretch"):
        start_process("sensor_air_quality")
        start_process("sensor_temperature")
        start_process("gateway")
        st.rerun()
    if action_cols[1].button("Desligar sensores e atuadores", width="stretch"):
        stop_all_devices()
        st.rerun()
    if action_cols[2].button("Desligar cliente web", width="stretch"):
        shutdown_client()
        st.rerun()


def read_log_tail(path, max_lines):
    try:
        lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    return lines[-max_lines:]


def collect_console_lines(selected_sources):
    lines = []
    if "Cliente Web" in selected_sources:
        lines.extend(st.session_state.client_logs[-MAX_CONSOLE_LINES:])

    for key, spec in PROCESS_SPECS.items():
        label = spec["label"]
        if label not in selected_sources:
            continue
        log_path = st.session_state.managed_log_files.get(key) or str(LOG_DIR / f"{key}.log")
        for line in read_log_tail(log_path, MAX_CONSOLE_LINES):
            lines.append(f"[{label}] {line}")

    return lines[-MAX_CONSOLE_LINES:]


def filter_video_console_lines(lines):
    process_lines = []
    discovery_lines = []
    reading_lines = []
    tcp_lines = []
    error_lines = []

    for line in lines:
        lower = line.lower()
        if any(token in lower for token in ["error", "erro", "falha", "recusado", "sem resposta"]):
            error_lines.append(line)
        elif any(token in line for token in ["TCP comando ->", "TCP resposta <-", "TCP control ->", "TCP control <-", "TCP command received"]):
            tcp_lines.append(line)
        elif "Sensor discovered:" in line or "Discovery response sent" in line:
            discovery_lines.append(line)
        elif "[Gateway] Reading " in line:
            reading_lines.append(line)
        elif any(token in line for token in ["===== ", "Iniciado ", "Desligado", "Conexao com Gateway restabelecida"]):
            process_lines.append(line)

    filtered = []
    filtered.extend(process_lines[-30:])
    filtered.extend(discovery_lines[-MAX_VIDEO_DISCOVERY_LINES:])
    filtered.extend(reading_lines[-MAX_VIDEO_READING_LINES:])
    filtered.extend(tcp_lines[-40:])
    filtered.extend(error_lines[-20:])
    return filtered[-MAX_VIDEO_CONSOLE_LINES:]


def clear_logs():
    st.session_state.client_logs = []
    LOG_DIR.mkdir(exist_ok=True)
    for path in LOG_DIR.glob("*.log"):
        try:
            path.write_text("", encoding="utf-8")
        except OSError:
            pass
    st.session_state.last_message = ("success", "Console de logs limpo.")


def render_log_console():
    st.subheader("Console de logs")
    st.caption("Use este painel no video para provar discovery multicast, leituras UDP, conexoes TCP e comandos.")

    source_options = ["Cliente Web"] + [spec["label"] for spec in PROCESS_SPECS.values()]
    selected_sources = st.multiselect(
        "Fontes de log",
        source_options,
        default=source_options,
    )

    cols = st.columns([1, 1, 4])
    if cols[0].button("Limpar logs", width="stretch"):
        clear_logs()
        st.rerun()
    if cols[1].button("Atualizar logs", width="stretch"):
        st.rerun()

    lines = collect_console_lines(selected_sources)
    detailed = st.checkbox("Mostrar logs detalhados", value=False)
    if not detailed:
        lines = filter_video_console_lines(lines)

    if not lines:
        st.info("Nenhum log importante ainda. Inicie processos ou envie comandos; use logs detalhados para depuracao.")
        return

    console_text = "\n".join(lines)
    st.code(console_text, language="text", line_numbers=False)


def render_analytics(host, port):
    st.subheader("Monitoramento de dados")
    metric_specs = [
        ("Media temperatura", messages_pb2.AVG_TEMPERATURE, "C"),
        ("Media CO2", messages_pb2.AVG_CO2, "ppm"),
        ("Media umidade", messages_pb2.AVG_HUMIDITY, "%"),
        ("Maior leitura", messages_pb2.MAX_READING, ""),
    ]
    cols = st.columns(4)
    for col, (label, request_type, unit) in zip(cols, metric_specs):
        resp, error = send_simple_request(host, port, request_type)
        if error:
            col.metric(label, "indisponivel")
        elif resp.status == messages_pb2.OK:
            suffix = f" {unit}" if unit else ""
            col.metric(label, f"{resp.metric_value:.2f}{suffix}")
            if request_type == messages_pb2.MAX_READING:
                col.caption(resp.message)
        else:
            col.metric(label, "sem dados")
            col.caption(resp.message)


def render_history_chart(host, port):
    resp, error = send_simple_request(host, port, messages_pb2.READING_HISTORY)
    if error or resp.status != messages_pb2.OK or not resp.readings:
        st.info("Historico ainda indisponivel para grafico.")
        return

    rows = []
    for reading in resp.readings:
        if reading.metric not in {"temperature", "co2", "humidity"}:
            continue
        rows.append(
            {
                "tempo": pd.to_datetime(reading.timestamp_unix_ms, unit="ms"),
                "metrica": reading.metric,
                "valor": reading.value,
            }
        )

    if not rows:
        st.info("Aguardando leituras de temperatura, CO2 ou umidade.")
        return

    df = pd.DataFrame(rows).sort_values(["metrica", "tempo"])
    metric_labels = {
        "temperature": "Temperatura (C)",
        "co2": "CO2 (ppm)",
        "humidity": "Umidade (%)",
    }
    df["metrica_label"] = df["metrica"].map(metric_labels).fillna(df["metrica"])

    st.caption("Series temporais dos sensores continuos")
    for metric in ["temperature", "co2", "humidity"]:
        metric_df = df[df["metrica"] == metric]
        if metric_df.empty:
            continue

        chart = (
            alt.Chart(metric_df)
            .mark_line(point=True)
            .encode(
                x=alt.X("tempo:T", title="Tempo"),
                y=alt.Y("valor:Q", title=metric_labels[metric], scale=alt.Scale(zero=False)),
                tooltip=[
                    alt.Tooltip("tempo:T", title="Tempo"),
                    alt.Tooltip("valor:Q", title="Valor", format=".2f"),
                    alt.Tooltip("metrica_label:N", title="Metrica"),
                ],
            )
            .properties(height=220)
        )
        st.altair_chart(chart, width="stretch")


def render_device_controls(host, port, sensor):
    controllable = sensor.device_kind == messages_pb2.ACTUATOR and sensor.control_tcp_port > 0

    with st.container(border=True):
        header_cols = st.columns([2, 1, 1, 1])
        header_cols[0].subheader(sensor.sensor_id)
        header_cols[1].write(DEVICE_TYPE_LABELS.get(sensor.sensor_type, str(sensor.sensor_type)))
        header_cols[2].write("Ativo" if sensor.is_active else "Inativo")
        header_cols[3].write("Atuador" if controllable else "Fonte de dados")

        detail_cols = st.columns(4)
        detail_cols[0].metric("IP", sensor.sensor_ip)
        detail_cols[1].metric("Porta TCP", sensor.control_tcp_port)
        detail_cols[2].metric("Categoria", DEVICE_KIND_LABELS.get(sensor.device_kind, "-"))
        detail_cols[3].metric("Tipo", DEVICE_TYPE_LABELS.get(sensor.sensor_type, "-"))
        st.caption(sensor.state_text or "Sem configuracao adicional")

        if not controllable:
            st.info("Sensores apenas enviam dados ao Gateway via UDP e nao recebem comandos TCP.")
            return

        base_cols = st.columns(2)
        if base_cols[0].button("Ligar", key=f"activate-{sensor.sensor_id}"):
            resp, error = send_control_command(host, port, sensor.sensor_id, messages_pb2.ACTIVATE)
            handle_command_result(resp, error)
            st.rerun()
        if base_cols[1].button("Desligar", key=f"deactivate-{sensor.sensor_id}"):
            resp, error = send_control_command(host, port, sensor.sensor_id, messages_pb2.DEACTIVATE)
            handle_command_result(resp, error)
            st.rerun()

        if sensor.sensor_type == messages_pb2.CAMERA:
            cols = st.columns(3)
            if cols[0].button("Iniciar gravacao", key=f"record-start-{sensor.sensor_id}"):
                resp, error = send_control_command(host, port, sensor.sensor_id, messages_pb2.CAMERA_START_RECORDING)
                handle_command_result(resp, error)
                st.rerun()
            if cols[1].button("Parar gravacao", key=f"record-stop-{sensor.sensor_id}"):
                resp, error = send_control_command(host, port, sensor.sensor_id, messages_pb2.CAMERA_STOP_RECORDING)
                handle_command_result(resp, error)
                st.rerun()
            direction = cols[2].selectbox(
                "Direcao",
                ["Norte", "Sul", "Leste", "Oeste"],
                key=f"direction-{sensor.sensor_id}",
            )
            if cols[2].button("Alterar direcao", key=f"direction-btn-{sensor.sensor_id}"):
                resp, error = send_control_command(
                    host, port, sensor.sensor_id, messages_pb2.CAMERA_SET_DIRECTION, text_value=direction
                )
                handle_command_result(resp, error)
                st.rerun()

        elif sensor.sensor_type == messages_pb2.TRAFFIC_LIGHT:
            cols = st.columns(2)
            color = cols[0].selectbox(
                "Cor",
                ["vermelho", "amarelo", "verde"],
                key=f"color-{sensor.sensor_id}",
            )
            if cols[0].button("Alterar cor", key=f"color-btn-{sensor.sensor_id}"):
                resp, error = send_control_command(
                    host, port, sensor.sensor_id, messages_pb2.TRAFFIC_LIGHT_SET_COLOR, text_value=color
                )
                handle_command_result(resp, error)
                st.rerun()
            mode = cols[1].selectbox(
                "Modo",
                ["automatico", "manual"],
                key=f"mode-{sensor.sensor_id}",
            )
            if cols[1].button("Alterar modo", key=f"mode-btn-{sensor.sensor_id}"):
                resp, error = send_control_command(
                    host, port, sensor.sensor_id, messages_pb2.TRAFFIC_LIGHT_SET_MODE, text_value=mode
                )
                handle_command_result(resp, error)
                st.rerun()

        elif sensor.sensor_type == messages_pb2.STREET_LIGHT:
            brightness = st.slider("Brilho (%)", min_value=0, max_value=100, value=70, key=f"brightness-{sensor.sensor_id}")
            if st.button("Alterar brilho", key=f"brightness-btn-{sensor.sensor_id}"):
                resp, error = send_control_command(
                    host, port, sensor.sensor_id, messages_pb2.STREET_LIGHT_SET_BRIGHTNESS, value=float(brightness)
                )
                handle_command_result(resp, error)
                st.rerun()


def render_client_panel(host, port):
    st.subheader("Cliente conectado ao Gateway")
    resp, error = list_sensors(host, int(port))
    if error:
        st.error(error)
        st.info("Com o Gateway desligado, o cliente web permanece aberto, mas nao consegue consultar estados nem enviar comandos.")
        return

    if resp.status != messages_pb2.OK:
        st.error(resp.message)
        return

    st.success("Conectado ao Gateway")

    devices = list(resp.sensors)
    if not devices:
        st.warning("Nenhum sensor descoberto ainda. Inicie sensores e aguarde o multicast do Gateway.")
        return

    st.subheader("Descoberta e estados dos dispositivos")
    st.dataframe([sensor_to_dict(sensor) for sensor in devices], width="stretch", hide_index=True)

    render_analytics(host, port)
    render_history_chart(host, port)

    st.subheader("Comandos para dispositivos especificos")
    for sensor in devices:
        render_device_controls(host, int(port), sensor)


def render_script_guide():
    with st.expander("Roteiro de teste e demonstracao"):
        st.markdown(
            """
1. Em **Processos**, inicie dois dispositivos: um atuador controlavel, por exemplo **Semaforo**, e um sensor continuo, por exemplo **Sensor de temperatura**.
2. Em **Processos**, inicie o **Gateway Inteligente**.
3. Em **Cliente analitico**, mostre se a descoberta esta funcionando pela tabela de dispositivos.
4. Mostre que o **Cliente Web** conectou ao Gateway pela mensagem **Conectado ao Gateway**.
5. Consulte os estados dos dispositivos conectados na tabela **Descoberta e estados dos dispositivos**.
6. Envie comandos para dispositivos especificos, por exemplo alterar a cor do semaforo ou ligar/desligar o poste.
7. Monitore os dados de algum sensor continuo em **Monitoramento de dados** e no grafico de historico.
8. Em **Processos**, desligue o processo de algum dispositivo, por exemplo o sensor de temperatura.
9. Em **Cliente analitico**, aguarde o estado do dispositivo ficar inativo.
10. Em **Processos**, inicie novamente o processo de um dispositivo.
11. Em **Processos**, desligue o **Gateway Inteligente**.
12. Em **Cliente analitico**, avalie o que acontece com o Cliente e os dispositivos: o cliente permanece aberto, mas mostra erro de conexao.
13. Em **Processos**, reinicie o **Gateway Inteligente**.
14. Em **Cliente analitico**, reconecte/aguarde o auto-refresh e mostre a redescoberta dos dispositivos.
15. Avalie o que acontece com o Cliente e os dispositivos apos o retorno do Gateway: estados voltam a aparecer e novas leituras chegam.
16. Em **Processos**, inicie o processo de outro dispositivo, por exemplo **Camera** ou **Luz do poste**.
17. Em **Cliente analitico**, consulte novamente os estados dos dispositivos conectados.
18. Envie novos comandos para dispositivos especificos, por exemplo iniciar gravacao da camera ou alterar brilho do poste.
19. Em **Processos**, clique em **Desligar sensores e atuadores**.
20. Em **Cliente analitico**, consulte os estados dos dispositivos apos o desligamento.
21. Finalize clicando em **Desligar** no **Gateway Inteligente** e depois em **Desligar cliente web**.

Use **Console de logs** entre as etapas para provar discovery multicast, leituras UDP, conexoes TCP e comandos enviados.
"""
        )


def main():
    ensure_process_state()
    st.set_page_config(page_title="Smart City - Cliente Web", layout="wide")
    st.title("Smart City - Cliente Web")
    st.caption("Cliente analitico em Streamlit usando TCP + Protocol Buffers.")

    with st.sidebar:
        st.header("Gateway")
        host = st.text_input("IP", value=DEFAULT_GATEWAY_IP)
        port = st.number_input("Porta TCP", min_value=1, max_value=65535, value=DEFAULT_GATEWAY_PORT)
        auto_refresh_default = st.session_state.get("auto_refresh_enabled", True)
        auto_refresh = st.checkbox("Atualizacao automatica", value=auto_refresh_default)
        st.session_state.auto_refresh_enabled = auto_refresh

        if st.button("Atualizar agora", width="stretch"):
            st.rerun()

        if auto_refresh:
            st_autorefresh(interval=AUTO_REFRESH_INTERVAL_MS, key="sensor-autorefresh")

    show_last_message()
    render_script_guide()

    tab_processes, tab_client, tab_logs = st.tabs(["Processos", "Cliente analitico", "Console de logs"])
    with tab_processes:
        render_process_panel()
    with tab_client:
        render_client_panel(host, int(port))
    with tab_logs:
        render_log_console()


if __name__ == "__main__":
    main()
