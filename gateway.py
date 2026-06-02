import socket
import struct
import threading
import time
from collections import defaultdict
from datetime import datetime
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


class Gateway:
    def __init__(self):
        # Aqui fica o "estado vivo" da cidade: sensores descobertos e histórico de leituras.
        self.sensors = {}
        self.readings_by_type = defaultdict(list)
        self.readings_all = []
        self.lock = threading.Lock()

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
                request_timestamp=datetime.utcnow().isoformat(),
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
                    self.readings_all.append(reading)
                    if reading.sensor_id in self.sensors:
                        self.sensors[reading.sensor_id]["last_seen"] = time.time()

                alert = f" ALERT={reading.alert} {reading.alert_message}" if reading.alert else ""
                print(f"[Gateway] Reading {reading.sensor_id} {reading.value} {reading.unit}{alert}")
            except Exception as exc:
                print(f"[Gateway] UDP error: {exc}")

    def multicast_response_server(self):
        # Porta separada para respostas de discovery dos sensores.
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("0.0.0.0", MULTICAST_PORT + 1))
        print(f"[Gateway] Discovery responses on 0.0.0.0:{MULTICAST_PORT + 1}")

        while True:
            try:
                data, addr = sock.recvfrom(65535)
                resp = messages_pb2.DiscoveryResponse()
                resp.ParseFromString(data)
                self.update_sensor(resp)
                print(f"[Gateway] Sensor discovered: {resp.sensor_id} ({resp.sensor_type}) at {addr[0]}")
            except Exception as exc:
                print(f"[Gateway] Discovery response error: {exc}")

    def send_control_command(self, sensor_id, command_type, value=0.0):
        # Cliente nunca conversa direto com sensor controlável; passa sempre pelo gateway.
        with self.lock:
            sensor = self.sensors.get(sensor_id)

        if not sensor:
            return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sensor nao encontrado")
        if sensor["control_tcp_port"] == 0:
            return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sensor nao controlavel")

        cmd = messages_pb2.ControlCommand(
            command_type=command_type,
            sensor_id=sensor_id,
            value=value,
        )

        try:
            with socket.create_connection((sensor["sensor_ip"], sensor["control_tcp_port"]), timeout=3) as conn:
                self.send_msg(conn, cmd.SerializeToString())
                raw = self.recv_msg(conn)
                if not raw:
                    return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sem resposta do sensor")

                ctrl_resp = messages_pb2.ControlResponse()
                ctrl_resp.ParseFromString(raw)

            # Mantém o estado local em sincronia com o que o sensor confirmou.
            with self.lock:
                if sensor_id in self.sensors:
                    self.sensors[sensor_id]["is_active"] = ctrl_resp.is_active
                    self.sensors[sensor_id]["frequency_seconds"] = ctrl_resp.frequency_seconds
                    self.sensors[sensor_id]["threshold"] = ctrl_resp.threshold

            return messages_pb2.ClientResponse(status=ctrl_resp.status, message=ctrl_resp.message)
        except Exception as exc:
            return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message=f"Falha no comando: {exc}")

    def build_sensor_list_response(self):
        resp = messages_pb2.ClientResponse(status=messages_pb2.OK, message="Sensores listados")
        with self.lock:
            for sensor in self.sensors.values():
                info = resp.sensors.add()
                info.sensor_id = sensor["sensor_id"]
                info.sensor_type = sensor["sensor_type"]
                info.sensor_ip = sensor["sensor_ip"]
                info.control_tcp_port = sensor["control_tcp_port"]
                info.is_active = sensor["is_active"]
                info.frequency_seconds = sensor["frequency_seconds"]
                info.threshold = sensor["threshold"]
        return resp

    def metric_average(self, sensor_type):
        with self.lock:
            vals = [r.value for r in self.readings_by_type[sensor_type]]
        if not vals:
            return None
        return sum(vals) / len(vals)

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
            avg = self.metric_average(messages_pb2.SENSOR_TEMPERATURE)
            if avg is None:
                return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sem leituras de temperatura")
            return messages_pb2.ClientResponse(status=messages_pb2.OK, message="Media de temperatura", metric_value=avg)

        if req.request_type == messages_pb2.AVG_CO2:
            avg = self.metric_average(messages_pb2.SENSOR_AIR_QUALITY)
            if avg is None:
                return messages_pb2.ClientResponse(status=messages_pb2.ERROR, message="Sem leituras de CO2")
            return messages_pb2.ClientResponse(status=messages_pb2.OK, message="Media de CO2", metric_value=avg)

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
