import socket
import struct

SOCKET_PATH = "/tmp/vswitch-sniffer.sock"
HEADER_SIZE = 17

def receive_exact(sock, size):
    data = b""

    while len(data) < size:
        chunk = sock.recv(size - len(data))

        if not chunk:
            return None

        data += chunk

    return data


sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(SOCKET_PATH)

print("Sniffer connected. Waiting for mirrored frames...\n")

try:
    while True:
        header = receive_exact(sock, HEADER_SIZE)

        if header is None:
            print("Switch disconnected.")
            break

        length = struct.unpack("!H", header[0:2])[0]
        source_mac = header[2:8].hex(":")
        destination_mac = header[8:14].hex(":")
        vlan = struct.unpack("!H", header[14:16])[0]
        priority = header[16]

        payload = receive_exact(sock, length)

        if payload is None:
            print("Incomplete frame received.")
            break

        print("========== MIRRORED FRAME ==========")
        print("Source MAC:", source_mac)
        print("Destination MAC:", destination_mac)
        print("VLAN:", vlan)
        print("Priority:", priority)
        print("Payload:", payload.decode(errors="replace"))
        print("===================================\n")

except KeyboardInterrupt:
    print("\nSniffer stopped.")

finally:
    sock.close()