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
SENSOR_TYPE = messages_pb2.SENSOR_TEMPERATURE
GATEWAY_UDP_IP = "127.0.0.1"
GATEWAY_UDP_PORT = 9001
MULTICAST_GROUP = "224.1.1.1"
MULTICAST_PORT = 10000
DISCOVERY_RESPONSE_PORT = 10001


def discovery_listener():
    # Fica "escutando" o multicast para se apresentar quando o gateway chamar.
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

            resp = messages_pb2.DiscoveryResponse(
                sensor_id=SENSOR_ID,
                sensor_type=SENSOR_TYPE,
                sensor_ip="127.0.0.1",
                control_tcp_port=0,
                is_active=True,
                frequency_seconds=2.0,
                threshold=0.0,
            )
            sock.sendto(resp.SerializeToString(), ("127.0.0.1", DISCOVERY_RESPONSE_PORT))
        except Exception as exc:
            print(f"[Temp] discovery error: {exc}")


def send_readings():
    # Sensor simples: gera temperatura aleatória e manda por UDP periodicamente.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while True:
        value = round(random.uniform(18.0, 34.0), 2)
        reading = messages_pb2.SensorReading(
            sensor_id=SENSOR_ID,
            sensor_type=SENSOR_TYPE,
            value=value,
            unit="C",
            timestamp_unix_ms=int(time.time() * 1000),
            alert=False,
            alert_message="",
        )
        try:
            sock.sendto(reading.SerializeToString(), (GATEWAY_UDP_IP, GATEWAY_UDP_PORT))
            print(f"[Temp] {value} C")
        except Exception as exc:
            print(f"[Temp] send error: {exc}")
        time.sleep(2)


if __name__ == "__main__":
    threading.Thread(target=discovery_listener, daemon=True).start()
    send_readings()
