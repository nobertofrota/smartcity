import socket
import struct
from pathlib import Path
import sys

BASE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(BASE_DIR))
sys.path.insert(0, str(BASE_DIR / "generated"))
from generated import messages_pb2

GATEWAY_TCP_IP = "127.0.0.1"
GATEWAY_TCP_PORT = 9000


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def recv_msg(conn):
    # Mesmo protocolo de framing usado no gateway/atuadores.
    header = recv_exact(conn, 4)
    if not header:
        return None
    size = struct.unpack("!I", header)[0]
    return recv_exact(conn, size)


def send_msg(conn, payload):
    conn.sendall(struct.pack("!I", len(payload)) + payload)


def show_menu():
    print("\n=== Cliente Analitico ===")
    print("1 - Listar dispositivos conectados")
    print("2 - Consultar media de temperatura")
    print("3 - Consultar media de CO2")
    print("4 - Consultar media de umidade")
    print("5 - Consultar maior leitura registrada")
    print("6 - Ligar atuador")
    print("7 - Desligar atuador")
    print("8 - Alterar cor do semaforo")
    print("9 - Alterar brilho do poste")
    print("10 - Sair")


def build_request(option):
    req = messages_pb2.ClientRequest()

    mapping = {
        "1": messages_pb2.LIST_SENSORS,
        "2": messages_pb2.AVG_TEMPERATURE,
        "3": messages_pb2.AVG_CO2,
        "4": messages_pb2.AVG_HUMIDITY,
        "5": messages_pb2.MAX_READING,
        "6": messages_pb2.SEND_CONTROL_COMMAND,
        "7": messages_pb2.SEND_CONTROL_COMMAND,
        "8": messages_pb2.SEND_CONTROL_COMMAND,
        "9": messages_pb2.SEND_CONTROL_COMMAND,
        "10": messages_pb2.EXIT,
    }

    if option not in mapping:
        return None

    req.request_type = mapping[option]

    # Só pedimos campos extras quando a operação realmente precisa deles.
    if option in {"6", "7", "8", "9"}:
        req.target_sensor_id = input("Dispositivo ID: ").strip()
    if option == "6":
        req.command_type = messages_pb2.ACTIVATE
    if option == "7":
        req.command_type = messages_pb2.DEACTIVATE
    if option == "8":
        req.command_type = messages_pb2.TRAFFIC_LIGHT_SET_COLOR
        req.text_value = input("Cor (verde/amarelo/vermelho): ").strip()
    if option == "9":
        req.command_type = messages_pb2.STREET_LIGHT_SET_BRIGHTNESS
        req.value = float(input("Brilho (0-100): ").strip())

    return req


def print_response(resp):
    status_txt = "OK" if resp.status == messages_pb2.OK else "ERROR"
    print(f"\n[{status_txt}] {resp.message}")

    if resp.sensors:
        print("Dispositivos:")
        for s in resp.sensors:
            print(
                f"- id={s.sensor_id} type={s.sensor_type} ip={s.sensor_ip} "
                f"control_port={s.control_tcp_port} active={s.is_active} "
                f"kind={s.device_kind} state={s.state_text}"
            )

    if resp.metric_value != 0:
        print(f"Valor: {resp.metric_value:.2f}")


def main():
    try:
        conn = socket.create_connection((GATEWAY_TCP_IP, GATEWAY_TCP_PORT), timeout=5)
    except Exception as exc:
        print(f"Falha ao conectar no gateway: {exc}")
        return

    with conn:
        while True:
            try:
                show_menu()
                option = input("Escolha: ").strip()
                req = build_request(option)
                if not req:
                    print("Opcao invalida")
                    continue

                send_msg(conn, req.SerializeToString())
                raw = recv_msg(conn)
                if not raw:
                    print("Conexao encerrada pelo gateway")
                    break

                resp = messages_pb2.ClientResponse()
                resp.ParseFromString(raw)
                print_response(resp)

                if req.request_type == messages_pb2.EXIT:
                    break
            except ValueError:
                print("Entrada invalida")
            except Exception as exc:
                print(f"Erro no cliente: {exc}")
                break


if __name__ == "__main__":
    main()
