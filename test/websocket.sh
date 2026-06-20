#!/usr/bin/env bash

set -euo pipefail

. "$(dirname "$0")/common.sh"

SERVER_LOG="$(make_temp_file)"

run_ws_check() {
	python3 - <<'PY'
import base64
import hashlib
import os
import socket
import struct

host = "127.0.0.1"
port = 5000
key = base64.b64encode(os.urandom(16)).decode("ascii")
expected_accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()).decode("ascii")

def recv_exact(sock, n):
    chunks = []
    remaining = n
    while remaining > 0:
        data = sock.recv(remaining)
        if not data:
            raise RuntimeError("unexpected EOF")
        chunks.append(data)
        remaining -= len(data)
    return b"".join(chunks)

def read_http_response(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("unexpected EOF during handshake")
        data += chunk
    header_blob, rest = data.split(b"\r\n\r\n", 1)
    lines = header_blob.decode("ascii").split("\r\n")
    status_line = lines[0]
    headers = {}
    for line in lines[1:]:
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()
    return status_line, headers, rest

def build_masked_text_frame(text):
    payload = text.encode("utf-8")
    mask = os.urandom(4)
    masked = bytes(payload[i] ^ mask[i % 4] for i in range(len(payload)))
    frame = bytearray()
    frame.append(0x81)
    if len(payload) < 126:
        frame.append(0x80 | len(payload))
    else:
        raise RuntimeError("payload too large for this test")
    frame.extend(mask)
    frame.extend(masked)
    return bytes(frame)

def read_frame(sock, initial=b""):
    buffer = bytearray(initial)
    while len(buffer) < 2:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("unexpected EOF before frame header")
        buffer.extend(chunk)
    b0, b1 = buffer[0], buffer[1]
    fin = (b0 & 0x80) != 0
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    length = b1 & 0x7F
    index = 2
    if length == 126:
        while len(buffer) < index + 2:
            buffer.extend(recv_exact(sock, index + 2 - len(buffer)))
        length = struct.unpack("!H", buffer[index:index+2])[0]
        index += 2
    elif length == 127:
        while len(buffer) < index + 8:
            buffer.extend(recv_exact(sock, index + 8 - len(buffer)))
        length = struct.unpack("!Q", buffer[index:index+8])[0]
        index += 8
    if masked:
        while len(buffer) < index + 4:
            buffer.extend(recv_exact(sock, index + 4 - len(buffer)))
        mask = buffer[index:index+4]
        index += 4
    else:
        mask = None
    while len(buffer) < index + length:
        buffer.extend(recv_exact(sock, index + length - len(buffer)))
    payload = bytes(buffer[index:index+length])
    if mask is not None:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(length))
    return fin, opcode, payload

with socket.create_connection((host, port), timeout=5) as sock:
    request = (
        "GET /sock HTTP/1.1\r\n"
        "Host: 127.0.0.1:5000\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(request.encode("ascii"))
    status_line, headers, rest = read_http_response(sock)
    if status_line != "HTTP/1.1 101 Switching Protocols":
        raise SystemExit(f"unexpected status line: {status_line}")
    if headers.get("upgrade", "").lower() != "websocket":
        raise SystemExit("missing or invalid Upgrade header")
    if headers.get("connection", "").lower() != "upgrade":
        raise SystemExit("missing or invalid Connection header")
    if headers.get("sec-websocket-accept") != expected_accept:
        raise SystemExit("unexpected Sec-WebSocket-Accept value")

    sock.sendall(build_masked_text_frame("hello websocket"))
    fin, opcode, payload = read_frame(sock, rest)
    if not fin:
        raise SystemExit("received fragmented frame")
    if opcode != 0x1:
        raise SystemExit(f"unexpected opcode: {opcode}")
    if payload.decode("utf-8") != "hello websocket":
        raise SystemExit(f"unexpected payload: {payload!r}")
PY
}

trap cleanup_test EXIT

build_server
start_server
run_ws_check

echo "PASS: websocket checks"