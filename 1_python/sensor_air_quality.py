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
DEVICE_TYPE = messages_pb2.AIR_QUALITY_SENSOR
MULTICAST_GROUP = "224.1.1.1"
MULTICAST_PORT = 10000
DISCOVERY_RESPONSE_PORT = 10001
FREQUENCY_SECONDS = 2.0
CO2_THRESHOLD = 900.0

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
                sensor_type=DEVICE_TYPE,
                sensor_ip=get_local_ip(gateway_ip),
                control_tcp_port=0,
                is_active=True,
                frequency_seconds=FREQUENCY_SECONDS,
                threshold=CO2_THRESHOLD,
                device_kind=messages_pb2.SENSOR,
                state_text="CO2 e umidade via UDP",
            )
            sock.sendto(resp.SerializeToString(), (gateway_ip, discovery_response_port))
            print(f"[Air] Discovery response sent to {gateway_ip}:{discovery_response_port}")
        except Exception as exc:
            print(f"[Air] discovery error: {exc}")


def build_reading(metric, value, unit, alert=False, alert_message=""):
    return messages_pb2.SensorReading(
        sensor_id=SENSOR_ID,
        sensor_type=DEVICE_TYPE,
        value=value,
        unit=unit,
        timestamp_unix_ms=int(time.time() * 1000),
        alert=alert,
        alert_message=alert_message,
        metric=metric,
    )


def send_readings():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while True:
        with gateway_lock:
            gateway_udp_ip = gateway_state["udp_ip"]
            gateway_udp_port = gateway_state["udp_port"]

        if not gateway_udp_ip or not gateway_udp_port:
            time.sleep(1)
            continue

        co2 = round(random.uniform(400.0, 1200.0), 2)
        humidity = round(random.uniform(35.0, 85.0), 2)
        alert = co2 > CO2_THRESHOLD
        readings = [
            build_reading(
                "co2",
                co2,
                "ppm",
                alert,
                f"CO2 acima do limiar ({CO2_THRESHOLD:.2f})" if alert else "",
            ),
            build_reading("humidity", humidity, "%"),
        ]

        for reading in readings:
            try:
                sock.sendto(reading.SerializeToString(), (gateway_udp_ip, gateway_udp_port))
            except Exception as exc:
                print(f"[Air] send error: {exc}")

        suffix = " ALERTA" if alert else ""
        print(f"[Air] CO2={co2} ppm humidity={humidity}%{suffix}")
        time.sleep(FREQUENCY_SECONDS)


if __name__ == "__main__":
    threading.Thread(target=discovery_listener, daemon=True).start()
    send_readings()
