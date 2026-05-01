"""Tiny probe: send `post_mortem_dump` to the running smw debug server and
print the reply. Used to validate src/post_mortem.c on-demand path."""
import socket, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4377
s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
s.settimeout(3)
s.sendall(b"ping\n")
print("ping:", s.recv(2048).decode().strip())
s.sendall(b"post_mortem_dump\n")
print("dump:", s.recv(4096).decode().strip())
s.close()
