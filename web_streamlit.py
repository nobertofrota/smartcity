import os
import socket
import struct
import subprocess
import threading
from pathlib import Path
import sys

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

PROCESS_SPECS = {
    "gateway": {
        "label": "Gateway Inteligente",
        "script": "gateway.py",
        "kind": "gateway",
    },
    "sensor_air_quality": {
        "label": "Sensor de qualidade do ar (controlavel)",
        "script": "sensor_air_quality.py",
        "kind": "sensor",
    },
    "sensor_temperature": {
        "label": "Sensor de temperatura (continuo)",
        "script": "sensor_temperature.py",
        "kind": "sensor",
    },
    "sensor_noise": {
        "label": "Sensor de ruido (continuo)",
        "script": "sensor_noise.py",
        "kind": "sensor",
    },
}

SENSOR_TYPE_LABELS = {
    messages_pb2.SENSOR_TEMPERATURE: "Temperatura",
    messages_pb2.SENSOR_AIR_QUALITY: "Qualidade do ar",
    messages_pb2.SENSOR_NOISE: "Ruido",
}


def ensure_process_state():
    if "managed_processes" not in st.session_state:
        st.session_state.managed_processes = {}
    if "last_message" not in st.session_state:
        st.session_state.last_message = None


def process_is_running(process):
    return process is not None and process.poll() is None


def start_process(process_key):
    spec = PROCESS_SPECS[process_key]
    current = st.session_state.managed_processes.get(process_key)
    if process_is_running(current):
        st.session_state.last_message = ("success", f"{spec['label']} ja esta em execucao.")
        return

    script_path = BASE_DIR / spec["script"]
    process = subprocess.Popen(
        [sys.executable, "-u", str(script_path)],
        cwd=BASE_DIR,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    st.session_state.managed_processes[process_key] = process
    st.session_state.last_message = ("success", f"{spec['label']} iniciado.")


def stop_process(process_key):
    spec = PROCESS_SPECS[process_key]
    process = st.session_state.managed_processes.get(process_key)
    if not process_is_running(process):
        st.session_state.last_message = ("error", f"{spec['label']} nao esta em execucao pela interface.")
        return

    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)

    st.session_state.last_message = ("success", f"{spec['label']} desligado.")


def stop_all_sensors():
    for key, spec in PROCESS_SPECS.items():
        if spec["kind"] == "sensor":
            process = st.session_state.managed_processes.get(key)
            if process_is_running(process):
                process.terminate()
    for key, spec in PROCESS_SPECS.items():
        if spec["kind"] == "sensor":
            process = st.session_state.managed_processes.get(key)
            if process_is_running(process):
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
    st.session_state.last_message = ("success", "Todos os sensores iniciados pela interface foram desligados.")


def shutdown_client():
    st.session_state.auto_refresh_enabled = False
    st.session_state.last_message = ("success", "Cliente web sera encerrado em instantes.")
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
        with socket.create_connection((host, port), timeout=TCP_TIMEOUT_SECONDS) as conn:
            send_msg(conn, req.SerializeToString())
            raw = recv_msg(conn)
            if not raw:
                return None, "Gateway encerrou a conexao sem resposta"

            resp = messages_pb2.ClientResponse()
            resp.ParseFromString(raw)
            return resp, None
    except OSError as exc:
        return None, f"Falha ao conectar no Gateway: {exc}"
    except Exception as exc:
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


def sensor_to_dict(sensor):
    return {
        "ID": sensor.sensor_id,
        "Tipo": SENSOR_TYPE_LABELS.get(sensor.sensor_type, f"Tipo {sensor.sensor_type}"),
        "IP": sensor.sensor_ip,
        "Porta controle": sensor.control_tcp_port,
        "Estado": "Ativo" if sensor.is_active else "Inativo",
        "Frequencia (s)": round(sensor.frequency_seconds, 2),
        "Limiar": round(sensor.threshold, 2),
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
        return

    if resp.status == messages_pb2.OK:
        st.session_state.last_message = ("success", resp.message)
    else:
        st.session_state.last_message = ("error", resp.message)


def render_process_panel():
    st.subheader("Processos do roteiro")
    st.caption("Use estes botoes para gravar o video sem abrir terminais extras.")

    for key, spec in PROCESS_SPECS.items():
        process = st.session_state.managed_processes.get(key)
        running = process_is_running(process)

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
    if action_cols[1].button("Desligar todos os sensores", width="stretch"):
        stop_all_sensors()
        st.rerun()
    if action_cols[2].button("Desligar cliente web", width="stretch"):
        shutdown_client()
        st.rerun()


def render_analytics(host, port):
    st.subheader("Monitoramento de dados")
    metric_specs = [
        ("Media temperatura", messages_pb2.AVG_TEMPERATURE, "C"),
        ("Media CO2", messages_pb2.AVG_CO2, "ppm"),
        ("Maior leitura", messages_pb2.MAX_READING, ""),
    ]
    cols = st.columns(3)
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


def render_sensor_controls(host, port, sensor):
    controllable = sensor.control_tcp_port > 0

    with st.container(border=True):
        header_cols = st.columns([2, 1, 1, 1])
        header_cols[0].subheader(sensor.sensor_id)
        header_cols[1].write(SENSOR_TYPE_LABELS.get(sensor.sensor_type, str(sensor.sensor_type)))
        header_cols[2].write("Ativo" if sensor.is_active else "Inativo")
        header_cols[3].write("Controlavel" if controllable else "Nao controlavel")

        detail_cols = st.columns(4)
        detail_cols[0].metric("IP", sensor.sensor_ip)
        detail_cols[1].metric("Porta TCP", sensor.control_tcp_port)
        detail_cols[2].metric("Frequencia", f"{sensor.frequency_seconds:.2f}s")
        detail_cols[3].metric("Limiar", f"{sensor.threshold:.2f}")

        if not controllable:
            st.info("Este sensor nao possui porta TCP de controle.")
            return

        action_cols = st.columns(4)
        if action_cols[0].button("Ativar", key=f"activate-{sensor.sensor_id}"):
            resp, error = send_sensor_command(
                host,
                port,
                messages_pb2.ACTIVATE_SENSOR,
                sensor.sensor_id,
            )
            handle_command_result(resp, error)
            st.rerun()

        if action_cols[1].button("Desativar", key=f"deactivate-{sensor.sensor_id}"):
            resp, error = send_sensor_command(
                host,
                port,
                messages_pb2.DEACTIVATE_SENSOR,
                sensor.sensor_id,
            )
            handle_command_result(resp, error)
            st.rerun()

        new_frequency = action_cols[2].number_input(
            "Frequencia (s)",
            min_value=0.1,
            value=max(float(sensor.frequency_seconds), 0.1),
            step=0.5,
            key=f"frequency-value-{sensor.sensor_id}",
        )
        if action_cols[2].button("Alterar frequencia", key=f"frequency-{sensor.sensor_id}"):
            resp, error = send_sensor_command(
                host,
                port,
                messages_pb2.CHANGE_FREQUENCY,
                sensor.sensor_id,
                new_frequency,
            )
            handle_command_result(resp, error)
            st.rerun()

        new_threshold = action_cols[3].number_input(
            "Limiar",
            value=float(sensor.threshold),
            step=50.0,
            key=f"threshold-value-{sensor.sensor_id}",
        )
        if action_cols[3].button("Alterar limiar", key=f"threshold-{sensor.sensor_id}"):
            resp, error = send_sensor_command(
                host,
                port,
                messages_pb2.CHANGE_THRESHOLD,
                sensor.sensor_id,
                new_threshold,
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

    sensors = list(resp.sensors)
    if not sensors:
        st.warning("Nenhum sensor descoberto ainda. Inicie sensores e aguarde o multicast do Gateway.")
        return

    st.subheader("Descoberta e estados dos dispositivos")
    st.dataframe([sensor_to_dict(sensor) for sensor in sensors], width="stretch", hide_index=True)

    render_analytics(host, port)

    st.subheader("Comandos para dispositivos especificos")
    for sensor in sensors:
        render_sensor_controls(host, int(port), sensor)


def render_script_guide():
    with st.expander("Roteiro sugerido para o video pela interface"):
        st.markdown(
            """
1. Clique em **Iniciar** no sensor de ar e no sensor de temperatura.
2. Clique em **Iniciar** no Gateway.
3. Aguarde a tabela mostrar os dispositivos descobertos.
4. Use a tabela para consultar os estados.
5. Use os controles do `air-1` para ativar/desativar, mudar frequencia e limiar.
6. Observe as metricas em **Monitoramento de dados** atualizando.
7. Desligue um sensor continuo e aguarde ele aparecer inativo.
8. Inicie novamente o sensor e aguarde a redescoberta.
9. Desligue o Gateway e observe o erro de conexao no cliente.
10. Reinicie o Gateway e aguarde a reconexao.
11. Inicie outro dispositivo, consulte estados e envie novo comando.
12. Clique em **Desligar todos os sensores** e consulte os estados.
13. Desligue o Gateway e clique em **Desligar cliente web**.
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

    tab_processes, tab_client = st.tabs(["Processos", "Cliente analitico"])
    with tab_processes:
        render_process_panel()
    with tab_client:
        render_client_panel(host, int(port))


if __name__ == "__main__":
    main()
