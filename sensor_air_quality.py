import random
import socket
import struct
import threading
import time
from pathlib import Path
import sys

BASE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE_DIR))
sys.path.insert(0, str(BASE_DIR / "generated"))
from generated import messages_pb2

SENSOR_ID = "air-1"
SENSOR_TYPE = messages_pb2.SENSOR_AIR_QUALITY
GATEWAY_UDP_IP = "127.0.0.1"
GATEWAY_UDP_PORT = 9001
MULTICAST_GROUP = "224.1.1.1"
MULTICAST_PORT = 10000
DISCOVERY_RESPONSE_PORT = 10001
CONTROL_PORT = 9101

state = {
    "active": True,
    "frequency": 2.0,
    "threshold": 900.0,
}
state_lock = threading.Lock()


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def recv_msg(conn):
    # Mesmo framing usado no gateway: 4 bytes de tamanho + payload.
    header = recv_exact(conn, 4)
    if not header:
        return None
    size = struct.unpack("!I", header)[0]
    return recv_exact(conn, size)


def send_msg(conn, payload):
    conn.sendall(struct.pack("!I", len(payload)) + payload)


def discovery_listener():
    # Responde ao discovery do gateway e informa que este sensor aceita controle TCP.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", MULTICAST_PORT))
    mreq = struct.pack("4sl", socket.inet_aton(MULTICAST_GROUP), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    while True:
        try:
            data, _ = sock.recvfrom(65535)
            req = messages_pb2.DiscoveryRequest()
            req.ParseFromString(data)
            with state_lock:
                resp = messages_pb2.DiscoveryResponse(
                    sensor_id=SENSOR_ID,
                    sensor_type=SENSOR_TYPE,
                    sensor_ip="127.0.0.1",
                    control_tcp_port=CONTROL_PORT,
                    is_active=state["active"],
                    frequency_seconds=state["frequency"],
                    threshold=state["threshold"],
                )
            sock.sendto(resp.SerializeToString(), ("127.0.0.1", DISCOVERY_RESPONSE_PORT))
        except Exception as exc:
            print(f"[Air] discovery error: {exc}")


def control_server():
    # Servidor TCP local para receber comandos do gateway.
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", CONTROL_PORT))
    server.listen()
    print(f"[Air] control TCP on 0.0.0.0:{CONTROL_PORT}")

    while True:
        conn, _ = server.accept()
        threading.Thread(target=handle_control_conn, args=(conn,), daemon=True).start()


def handle_control_conn(conn):
    with conn:
        try:
            raw = recv_msg(conn)
            if not raw:
                return
            cmd = messages_pb2.ControlCommand()
            cmd.ParseFromString(raw)

            with state_lock:
                if cmd.command_type == messages_pb2.ACTIVATE:
                    state["active"] = True
                    msg = "Sensor ativado"
                elif cmd.command_type == messages_pb2.DEACTIVATE:
                    state["active"] = False
                    msg = "Sensor desativado"
                elif cmd.command_type == messages_pb2.SET_FREQUENCY:
                    if cmd.value <= 0:
                        raise ValueError("Frequencia deve ser maior que zero")
                    state["frequency"] = cmd.value
                    msg = f"Frequencia alterada para {cmd.value:.2f}s"
                elif cmd.command_type == messages_pb2.SET_THRESHOLD:
                    state["threshold"] = cmd.value
                    msg = f"Limiar alterado para {cmd.value:.2f} ppm"
                else:
                    raise ValueError("Comando invalido")

                resp = messages_pb2.ControlResponse(
                    status=messages_pb2.OK,
                    message=msg,
                    sensor_id=SENSOR_ID,
                    is_active=state["active"],
                    frequency_seconds=state["frequency"],
                    threshold=state["threshold"],
                )
            send_msg(conn, resp.SerializeToString())
        except Exception as exc:
            resp = messages_pb2.ControlResponse(
                status=messages_pb2.ERROR,
                message=f"Erro no comando: {exc}",
                sensor_id=SENSOR_ID,
                is_active=state["active"],
                frequency_seconds=state["frequency"],
                threshold=state["threshold"],
            )
            try:
                send_msg(conn, resp.SerializeToString())
            except Exception:
                pass


def send_readings():
    # Quando ativo, envia CO2 e dispara alerta quando passa do limiar configurado.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while True:
        with state_lock:
            active = state["active"]
            freq = state["frequency"]
            threshold = state["threshold"]

        if active:
            value = round(random.uniform(400.0, 1200.0), 2)
            alert = value > threshold
            reading = messages_pb2.SensorReading(
                sensor_id=SENSOR_ID,
                sensor_type=SENSOR_TYPE,
                value=value,
                unit="ppm",
                timestamp_unix_ms=int(time.time() * 1000),
                alert=alert,
                alert_message=(f"CO2 acima do limiar ({threshold:.2f})" if alert else ""),
            )
            try:
                sock.sendto(reading.SerializeToString(), (GATEWAY_UDP_IP, GATEWAY_UDP_PORT))
                suffix = " ALERTA" if alert else ""
                print(f"[Air] {value} ppm{suffix}")
            except Exception as exc:
                print(f"[Air] send error: {exc}")

        time.sleep(freq)


if __name__ == "__main__":
    threading.Thread(target=discovery_listener, daemon=True).start()
    threading.Thread(target=control_server, daemon=True).start()
    send_readings()
