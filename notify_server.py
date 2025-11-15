# save as notify_server.py
import socket

HOST = "127.0.0.1"
PORT = 9009

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(5)
    print(f"listening on {HOST}:{PORT} ...")

    while True:
        conn, addr = s.accept()
        print("accepted from", addr)
        with conn, conn.makefile("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                print("event:", line)