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

SENSOR_ID = "temp-1"
SENSOR_TYPE = messages_pb2.TEMPERATURE_SENSOR
MULTICAST_GROUP = "224.1.1.1"
MULTICAST_PORT = 10000
DISCOVERY_RESPONSE_PORT = 10001

gateway_state = {
    "udp_ip": None,
    "udp_port": None,
}
gateway_lock = threading.Lock()


def get_local_ip(remote_ip):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect((remote_ip, 1))
            return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def discovery_listener():
    # Fica "escutando" o multicast para se apresentar quando o gateway chamar.
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
            gateway_udp_port = req.gateway_udp_port or 9001
            discovery_response_port = req.discovery_response_port or DISCOVERY_RESPONSE_PORT

            with gateway_lock:
                gateway_state["udp_ip"] = gateway_ip
                gateway_state["udp_port"] = gateway_udp_port

            resp = messages_pb2.DiscoveryResponse(
                sensor_id=SENSOR_ID,
                sensor_type=SENSOR_TYPE,
                sensor_ip=get_local_ip(gateway_ip),
                control_tcp_port=0,
                is_active=True,
                frequency_seconds=2.0,
                threshold=0.0,
                device_kind=messages_pb2.SENSOR,
                state_text="Temperatura via UDP",
            )
            sock.sendto(resp.SerializeToString(), (gateway_ip, discovery_response_port))
            print(f"[Temp] Discovery response sent to {gateway_ip}:{discovery_response_port}")
        except Exception as exc:
            print(f"[Temp] discovery error: {exc}")


def send_readings():
    # Sensor simples: gera temperatura aleatória e manda por UDP periodicamente.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while True:
        with gateway_lock:
            gateway_udp_ip = gateway_state["udp_ip"]
            gateway_udp_port = gateway_state["udp_port"]

        if not gateway_udp_ip or not gateway_udp_port:
            time.sleep(1)
            continue

        value = round(random.uniform(18.0, 34.0), 2)
        reading = messages_pb2.SensorReading(
            sensor_id=SENSOR_ID,
            sensor_type=SENSOR_TYPE,
            value=value,
            unit="C",
            timestamp_unix_ms=int(time.time() * 1000),
            alert=False,
            alert_message="",
            metric="temperature",
        )
        try:
            sock.sendto(reading.SerializeToString(), (gateway_udp_ip, gateway_udp_port))
            print(f"[Temp] {value} C")
        except Exception as exc:
            print(f"[Temp] send error: {exc}")
        time.sleep(2)


if __name__ == "__main__":
    threading.Thread(target=discovery_listener, daemon=True).start()
    send_readings()
