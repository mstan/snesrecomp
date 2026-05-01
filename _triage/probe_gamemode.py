"""Quick GameMode read."""
import socket
s = socket.create_connection(("127.0.0.1", 4377), timeout=8)
s.settimeout(20)
buf = b""
while b"\n" not in buf:
    buf += s.recv(1<<16)
nl = buf.find(b"\n")
print("banner:", buf[:nl].decode())
buf = buf[nl+1:]
def cmd(line):
    global buf
    s.sendall((line + "\n").encode())
    while b"\n" not in buf:
        buf += s.recv(1<<16)
    nl = buf.find(b"\n")
    out = buf[:nl].decode()
    buf = buf[nl+1:]
    return out

# $7E:0100 is GameMode (also stamped at $0101..$010F as latches)
print("frame:", cmd("frame"))
print("GameMode $7E:0100:", cmd("read_ram 0100 1"))
print("StripeImage $7E:0012:", cmd("read_ram 0012 1"))
