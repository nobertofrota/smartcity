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
    # Mesmo protocolo de framing usado no gateway/sensor controlável.
    header = recv_exact(conn, 4)
    if not header:
        return None
    size = struct.unpack("!I", header)[0]
    return recv_exact(conn, size)


def send_msg(conn, payload):
    conn.sendall(struct.pack("!I", len(payload)) + payload)


def show_menu():
    print("\n=== Cliente Analitico ===")
    print("1 - Listar sensores conectados")
    print("2 - Consultar media de temperatura")
    print("3 - Consultar media de CO2")
    print("4 - Consultar maior leitura registrada")
    print("5 - Ativar sensor")
    print("6 - Desativar sensor")
    print("7 - Alterar frequencia de sensor")
    print("8 - Alterar limiar de alerta")
    print("9 - Sair")


def build_request(option):
    req = messages_pb2.ClientRequest()

    mapping = {
        "1": messages_pb2.LIST_SENSORS,
        "2": messages_pb2.AVG_TEMPERATURE,
        "3": messages_pb2.AVG_CO2,
        "4": messages_pb2.MAX_READING,
        "5": messages_pb2.ACTIVATE_SENSOR,
        "6": messages_pb2.DEACTIVATE_SENSOR,
        "7": messages_pb2.CHANGE_FREQUENCY,
        "8": messages_pb2.CHANGE_THRESHOLD,
        "9": messages_pb2.EXIT,
    }

    if option not in mapping:
        return None

    req.request_type = mapping[option]

    # Só pedimos campos extras quando a operação realmente precisa deles.
    if option in {"5", "6", "7", "8"}:
        req.target_sensor_id = input("Sensor ID: ").strip()
    if option in {"7", "8"}:
        req.value = float(input("Novo valor: ").strip())

    return req


def print_response(resp):
    status_txt = "OK" if resp.status == messages_pb2.OK else "ERROR"
    print(f"\n[{status_txt}] {resp.message}")

    if resp.sensors:
        print("Sensores:")
        for s in resp.sensors:
            print(
                f"- id={s.sensor_id} type={s.sensor_type} ip={s.sensor_ip} "
                f"control_port={s.control_tcp_port} active={s.is_active} "
                f"freq={s.frequency_seconds:.2f}s threshold={s.threshold:.2f}"
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
