import socket
import struct
import threading
from pathlib import Path
import sys

BASE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE_DIR))
sys.path.insert(0, str(BASE_DIR / "generated"))
from generated import messages_pb2

DEVICE_ID = "camera-1"
DEVICE_TYPE = messages_pb2.CAMERA
CONTROL_PORT = 9201
MULTICAST_GROUP = "224.1.1.1"
MULTICAST_PORT = 10000
DISCOVERY_RESPONSE_PORT = 10001

state = {
    "active": True,
    "recording": False,
    "direction": "Norte",
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
    header = recv_exact(conn, 4)
    if not header:
        return None
    size = struct.unpack("!I", header)[0]
    return recv_exact(conn, size)


def send_msg(conn, payload):
    conn.sendall(struct.pack("!I", len(payload)) + payload)


def state_text():
    recording = "gravando" if state["recording"] else "sem gravacao"
    active = "ligada" if state["active"] else "desligada"
    return f"{active}; {recording}; direcao={state['direction']}"


def get_local_ip(remote_ip):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect((remote_ip, 1))
            return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def discovery_listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", MULTICAST_PORT))
    mreq = struct.pack("4sl", socket.inet_aton(MULTICAST_GROUP), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    while True:
        try:
            data, addr = sock.recvfrom(65535)
            req = messages_pb2.DiscoveryRequest()
            req.ParseFromString(data)
            gateway_ip = addr[0]
            response_port = req.discovery_response_port or DISCOVERY_RESPONSE_PORT

            with state_lock:
                resp = messages_pb2.DiscoveryResponse(
                    sensor_id=DEVICE_ID,
                    sensor_type=DEVICE_TYPE,
                    sensor_ip=get_local_ip(gateway_ip),
                    control_tcp_port=CONTROL_PORT,
                    is_active=state["active"],
                    device_kind=messages_pb2.ACTUATOR,
                    state_text=state_text(),
            )
            sock.sendto(resp.SerializeToString(), (gateway_ip, response_port))
            print(f"[Camera] Discovery response sent to {gateway_ip}:{response_port} state={state_text()}")
        except Exception as exc:
            print(f"[Camera] discovery error: {exc}")


def control_server():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", CONTROL_PORT))
    server.listen()
    print(f"[Camera] control TCP on 0.0.0.0:{CONTROL_PORT}")

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
            print(
                f"[Camera] TCP command received command={cmd.command_type} "
                f"value={cmd.value} text={cmd.text_value}"
            )

            with state_lock:
                if cmd.command_type == messages_pb2.ACTIVATE:
                    state["active"] = True
                    msg = "Camera ligada"
                elif cmd.command_type == messages_pb2.DEACTIVATE:
                    state["active"] = False
                    state["recording"] = False
                    msg = "Camera desligada"
                elif cmd.command_type == messages_pb2.CAMERA_START_RECORDING:
                    if not state["active"]:
                        raise ValueError("Camera desligada")
                    state["recording"] = True
                    msg = "Gravacao iniciada"
                elif cmd.command_type == messages_pb2.CAMERA_STOP_RECORDING:
                    state["recording"] = False
                    msg = "Gravacao parada"
                elif cmd.command_type == messages_pb2.CAMERA_SET_DIRECTION:
                    if not cmd.text_value:
                        raise ValueError("Direcao obrigatoria")
                    state["direction"] = cmd.text_value
                    msg = f"Direcao alterada para {cmd.text_value}"
                else:
                    raise ValueError("Comando invalido para camera")

                resp = messages_pb2.ControlResponse(
                    status=messages_pb2.OK,
                    message=msg,
                    sensor_id=DEVICE_ID,
                    is_active=state["active"],
                    state_text=state_text(),
                )
            send_msg(conn, resp.SerializeToString())
        except Exception as exc:
            with state_lock:
                resp = messages_pb2.ControlResponse(
                    status=messages_pb2.ERROR,
                    message=f"Erro no comando: {exc}",
                    sensor_id=DEVICE_ID,
                    is_active=state["active"],
                    state_text=state_text(),
                )
            send_msg(conn, resp.SerializeToString())


if __name__ == "__main__":
    threading.Thread(target=discovery_listener, daemon=True).start()
    control_server()
