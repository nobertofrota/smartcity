import csv
import socket
import struct
import threading
import time
from collections import defaultdict
from datetime import UTC, datetime
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
import sys

sys.path.insert(0, str(BASE_DIR))
sys.path.insert(0, str(BASE_DIR / "generated"))

from generated import messages_pb2

GATEWAY_ID = "gateway-1"
GATEWAY_TCP_HOST = "0.0.0.0"
GATEWAY_TCP_PORT = 9000
GATEWAY_UDP_HOST = "0.0.0.0"
GATEWAY_UDP_PORT = 9001
MULTICAST_GROUP = "224.1.1.1"
MULTICAST_PORT = 10000
DISCOVERY_RESPONSE_PORT = MULTICAST_PORT + 1
SENSOR_STALE_GRACE_SECONDS = 10.0
CSV_DIR = BASE_DIR / "data" / "csv"


class Gateway:
    def __init__(self):
        # Aqui fica o "estado vivo" da cidade: sensores descobertos e histórico de leituras.
        self.sensors = {}
        self.readings_by_type = defaultdict(list)
        self.readings_by_metric = defaultdict(list)
        self.readings_all = []
        self.lock = threading.Lock()
        CSV_DIR.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def recv_msg(conn):
        # Framing TCP simples: 4 bytes com tamanho + payload protobuf.
        header = Gateway.recv_exact(conn, 4)
        if not header:
            return None
        size = struct.unpack("!I", header)[0]
        return Gateway.recv_exact(conn, size)

    @staticmethod
    def recv_exact(conn, n):
        data = b""
        while len(data) < n:
            chunk = conn.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    @staticmethod
    def send_msg(conn, payload):
        conn.sendall(struct.pack("!I", len(payload)) + payload)

    @staticmethod
    def _safe_filename(device_id):
        safe = "".join(ch if ch.isalnum() or ch in {"-", "_"} else "_" for ch in device_id)
        return safe or "unknown"

    def persist_reading_csv(self, reading):
        csv_path = CSV_DIR / f"{self._safe_filename(reading.sensor_id)}.csv"
        file_exists = csv_path.exists()
        with csv_path.open("a", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            if not file_exists:
                writer.writerow([
                    "sensor_id",
                    "sensor_type",
                    "metric",
                    "value",
                    "unit",
                    "timestamp_unix_ms",
                    "alert",
                    "alert_message",
                ])
            writer.writerow([
                reading.sensor_id,
                reading.sensor_type,
                reading.metric,
                reading.value,
                reading.unit,
                reading.timestamp_unix_ms,
                reading.alert,
                reading.alert_message,
            ])

    def update_sensor(self, discovery):
        # Sempre que um sensor responde ao discovery, atualizamos seu cadastro local.
        sensor = {
            "sensor_id": discovery.sensor_id,
            "sensor_type": discovery.sensor_type,
            "sensor_ip": discovery.sensor_ip,
            "control_tcp_port": discovery.control_tcp_port,
            "is_active": discovery.is_active,
            "frequency_seconds": discovery.frequency_seconds,
            "threshold": discovery.threshold,
            "device_kind": discovery.device_kind,
            "state_text": discovery.state_text,
            "last_seen": time.time(),
        }
        with self.lock:
            self.sensors[discovery.sensor_id] = sensor

    def discovery_loop(self):
        # Pergunta multicast periódica: "sensores, se apresentem".
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)

        while True:
            req = messages_pb2.DiscoveryRequest(
                gateway_id=GATEWAY_ID,
                request_timestamp=datetime.now(UTC).isoformat(),
                gateway_udp_port=GATEWAY_UDP_PORT,
                discovery_response_port=DISCOVERY_RESPONSE_PORT,
            )
            sock.sendto(req.SerializeToString(), (MULTICAST_GROUP, MULTICAST_PORT))
            time.sleep(5)

    def udp_server(self):
        # Canal UDP principal para ingestão de leituras.
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((GATEWAY_UDP_HOST, GATEWAY_UDP_PORT))
        print(f"[Gateway] UDP listening on {GATEWAY_UDP_HOST}:{GATEWAY_UDP_PORT}")

        while True:
            try:
                data, _ = sock.recvfrom(65535)
                reading = messages_pb2.SensorReading()
                reading.ParseFromString(data)

                with self.lock:
                    self.readings_by_type[reading.sensor_type].append(reading)
                    self.readings_by_metric[reading.metric].append(reading)
                    self.readings_all.append(reading)
                    if reading.sensor_id in self.sensors:
                        self.sensors[reading.sensor_id]["last_seen"] = time.time()

                self.persist_reading_csv(reading)

                alert = f" ALERT={reading.alert} {reading.alert_message}" if reading.alert else ""
                print(f"[Gateway] Reading {reading.sensor_id} {reading.metric}={reading.value} {reading.unit}{alert}")
            except Exception as exc:
                print(f"[Gateway] UDP error: {exc}")

    def multicast_response_server(self):
        # Porta separada para respostas de discovery dos sensores.
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("0.0.0.0", DISCOVERY_RESPONSE_PORT))
        print(f"[Gateway] Discovery responses on 0.0.0.0:{DISCOVERY_RESPONSE_PORT}")

        while True:
            try:
                data, addr = sock.recvfrom(65535)
                resp = messages_pb2.DiscoveryResponse()
                resp.ParseFromString(data)
                self.update_sensor(resp)
                print(f"[Gateway] Sensor discovered: {resp.sensor_id} ({resp.sensor_type}) at {addr[0]}")
            except Exception as exc:
                print(f"[Gateway] Discovery response error: {exc}")

    def send_control_command(self, sensor_id, command_type, value=0.0, text_value=""):
        # Cliente nunca conversa direto com atuador; passa sempre pelo gateway.
        with self.lock:
            sensor = self.sensors.get(sensor_id)

        if not sensor:
            return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Dispositivo nao encontrado")
        if sensor["device_kind"] != messages_pb2.ACTUATOR or sensor["control_tcp_port"] == 0:
            return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Fonte de dados nao controlavel")

        cmd = messages_pb2.ControlCommand(
            command_type=command_type,
            sensor_id=sensor_id,
            value=value,
            text_value=text_value,
        )

        try:
            print(
                f"[Gateway] TCP control -> {sensor_id} "
                f"command={command_type} value={value} text={text_value}"
            )
            with socket.create_connection((sensor["sensor_ip"], sensor["control_tcp_port"]), timeout=3) as conn:
                self.send_msg(conn, cmd.SerializeToString())
                raw = self.recv_msg(conn)
                if not raw:
                    return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sem resposta do sensor")

                ctrl_resp = messages_pb2.ControlResponse()
                ctrl_resp.ParseFromString(raw)
                print(f"[Gateway] TCP control <- {sensor_id} status={ctrl_resp.status} message={ctrl_resp.message}")

            # Mantém o estado local em sincronia com o que o sensor confirmou.
            with self.lock:
                if sensor_id in self.sensors:
                    self.sensors[sensor_id]["is_active"] = ctrl_resp.is_active
                    self.sensors[sensor_id]["frequency_seconds"] = ctrl_resp.frequency_seconds
                    self.sensors[sensor_id]["threshold"] = ctrl_resp.threshold
                    self.sensors[sensor_id]["state_text"] = ctrl_resp.state_text
                    self.sensors[sensor_id]["last_seen"] = time.time()

            return messages_pb2.ClientResponse(status=ctrl_resp.status, message=ctrl_resp.message)
        except Exception as exc:
            return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message=f"Falha no comando: {exc}")

    def build_sensor_list_response(self):
        resp = messages_pb2.ClientResponse(status=messages_pb2.OK, message="Sensores listados")
        now = time.time()
        with self.lock:
            for sensor in self.sensors.values():
                timeout = max(SENSOR_STALE_GRACE_SECONDS, sensor["frequency_seconds"] * 3)
                recently_seen = now - sensor["last_seen"] <= timeout
                info = resp.sensors.add()
                info.sensor_id = sensor["sensor_id"]
                info.sensor_type = sensor["sensor_type"]
                info.sensor_ip = sensor["sensor_ip"]
                info.control_tcp_port = sensor["control_tcp_port"]
                info.is_active = sensor["is_active"] and recently_seen
                info.frequency_seconds = sensor["frequency_seconds"]
                info.threshold = sensor["threshold"]
                info.device_kind = sensor["device_kind"]
                info.state_text = sensor["state_text"]
        return resp

    def metric_average(self, metric):
        with self.lock:
            vals = [r.value for r in self.readings_by_metric[metric]]
        if not vals:
            return None
        return sum(vals) / len(vals)

    def metric_history(self):
        resp = messages_pb2.ClientResponse(status=messages_pb2.OK, message="Historico de leituras")
        with self.lock:
            resp.readings.extend(self.readings_all[-200:])
        return resp

    def metric_max(self):
        with self.lock:
            if not self.readings_all:
                return None
            return max(self.readings_all, key=lambda r: r.value)

    def process_client_request(self, req):
        # Dispatcher das opções do menu do cliente analítico.
        if req.request_type == messages_pb2.LIST_SENSORS:
            return self.build_sensor_list_response()

        if req.request_type == messages_pb2.AVG_TEMPERATURE:
            avg = self.metric_average("temperature")
            if avg is None:
                return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sem leituras de temperatura")
            return messages_pb2.ClientResponse(status=messages_pb2.OK, message="Media de temperatura", metric_value=avg)

        if req.request_type == messages_pb2.AVG_CO2:
            avg = self.metric_average("co2")
            if avg is None:
                return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sem leituras de CO2")
            return messages_pb2.ClientResponse(status=messages_pb2.OK, message="Media de CO2", metric_value=avg)

        if req.request_type == messages_pb2.AVG_HUMIDITY:
            avg = self.metric_average("humidity")
            if avg is None:
                return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sem leituras de umidade")
            return messages_pb2.ClientResponse(status=messages_pb2.OK, message="Media de umidade", metric_value=avg)

        if req.request_type == messages_pb2.MAX_READING:
            mx = self.metric_max()
            if mx is None:
                return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sem leituras")
            return messages_pb2.ClientResponse(
                status=messages_pb2.OK,
                message=f"Maior leitura: {mx.sensor_id} {mx.value} {mx.unit}",
                metric_value=mx.value,
            )

        if req.request_type == messages_pb2.ACTIVATE_SENSOR:
            return self.send_control_command(req.target_sensor_id, messages_pb2.ACTIVATE)

        if req.request_type == messages_pb2.DEACTIVATE_SENSOR:
            return self.send_control_command(req.target_sensor_id, messages_pb2.DEACTIVATE)

        if req.request_type == messages_pb2.CHANGE_FREQUENCY:
            return self.send_control_command(req.target_sensor_id, messages_pb2.SET_FREQUENCY, req.value)

        if req.request_type == messages_pb2.CHANGE_THRESHOLD:
            return self.send_control_command(req.target_sensor_id, messages_pb2.SET_THRESHOLD, req.value)

        if req.request_type == messages_pb2.READING_HISTORY:
            return self.metric_history()

        if req.request_type == messages_pb2.SEND_CONTROL_COMMAND:
            return self.send_control_command(req.target_sensor_id, req.command_type, req.value, req.text_value)

        if req.request_type == messages_pb2.EXIT:
            return messages_pb2.ClientResponse(status=messages_pb2.OK, message="Conexao encerrada")

        return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Requisicao invalida")

    def tcp_server(self):
        # Porta TCP para os clientes analíticos.
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((GATEWAY_TCP_HOST, GATEWAY_TCP_PORT))
        server.listen()
        print(f"[Gateway] TCP listening on {GATEWAY_TCP_HOST}:{GATEWAY_TCP_PORT}")

        while True:
            conn, addr = server.accept()
            print(f"[Gateway] Client connected: {addr}")
            threading.Thread(target=self.handle_client, args=(conn, addr), daemon=True).start()

    def handle_client(self, conn, addr):
        with conn:
            while True:
                try:
                    raw = self.recv_msg(conn)
                    if not raw:
                        break

                    req = messages_pb2.ClientRequest()
                    req.ParseFromString(raw)
                    resp = self.process_client_request(req)
                    self.send_msg(conn, resp.SerializeToString())

                    if req.request_type == messages_pb2.EXIT:
                        break
                except Exception as exc:
                    err = messages_pb2.ClientResponse(status=messages_pb2.ERROR, message=f"Erro: {exc}")
                    try:
                        self.send_msg(conn, err.SerializeToString())
                    except Exception:
                        pass
                    break
        print(f"[Gateway] Client disconnected: {addr}")

    def start(self):
        # O gateway precisa ouvir tudo ao mesmo tempo, por isso usamos threads.
        threading.Thread(target=self.udp_server, daemon=True).start()
        threading.Thread(target=self.multicast_response_server, daemon=True).start()
        threading.Thread(target=self.discovery_loop, daemon=True).start()
        self.tcp_server()


if __name__ == "__main__":
    Gateway().start()
