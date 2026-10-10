#!/usr/bin/env python3
# Copyright (c) 2026 J.J. Boyd. Original diagnostic source, authored with Codex.
# SPDX-License-Identifier: MIT
"""Test TLS and RV WebSocket hello separately over IPv4 and IPv6.

Does not register a station, pair a device, or open a radio session.
"""
import base64
import hashlib
import os
import socket
import ssl
import struct
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "rv.nereussdr.com"

def receive_exact(connection, count):
    output = bytearray()
    while len(output) < count:
        part = connection.recv(count - len(output))
        if not part:
            raise EOFError("connection closed before WebSocket hello")
        output.extend(part)
    return bytes(output)

def probe(family, label):
    print(f"\n{label}: {host}:443", flush=True)
    try:
        addresses = socket.getaddrinfo(host, 443, family, socket.SOCK_STREAM)
    except OSError as error:
        print(f"DNS unavailable: {error}", flush=True)
        return False
    seen = set()
    deadline = time.monotonic() + 20
    for _, _, _, _, address in addresses:
        if address in seen:
            continue
        seen.add(address)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        print(f"Trying {address}", flush=True)
        try:
            with socket.socket(family, socket.SOCK_STREAM) as raw:
                raw.settimeout(min(6, remaining))
                raw.connect(address)
                with ssl.create_default_context().wrap_socket(raw, server_hostname=host) as connection:
                    print(f"TLS verified; local={connection.getsockname()} peer={connection.getpeername()}", flush=True)
                    key = base64.b64encode(os.urandom(16)).decode("ascii")
                    request = (f"GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
                               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                               "Sec-WebSocket-Version: 13\r\n\r\n")
                    connection.sendall(request.encode("ascii"))
                    header = bytearray()
                    while not header.endswith(b"\r\n\r\n"):
                        if len(header) >= 16384:
                            raise ValueError("HTTP header too large")
                        header.extend(receive_exact(connection, 1))
                    lines = header.decode("iso-8859-1").split("\r\n")
                    print(lines[0], flush=True)
                    if lines[0].split()[1] != "101":
                        raise ValueError("WebSocket upgrade rejected")
                    fields = {line.split(":", 1)[0].lower(): line.split(":", 1)[1].strip()
                              for line in lines[1:] if ":" in line}
                    accept = base64.b64encode(hashlib.sha1(
                        (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
                    ).digest()).decode("ascii")
                    if fields.get("sec-websocket-accept") != accept:
                        raise ValueError("WebSocket accept hash mismatch")
                    first, second = receive_exact(connection, 2)
                    if first != 0x81 or second & 0x80:
                        raise ValueError("expected one unmasked text hello frame")
                    length = second & 0x7f
                    if length == 126:
                        length = struct.unpack("!H", receive_exact(connection, 2))[0]
                    elif length == 127:
                        length = struct.unpack("!Q", receive_exact(connection, 8))[0]
                    if length > 65536:
                        raise ValueError("hello frame too large")
                    # Only report its type; don't publish service nonce or credentials.
                    import json
                    hello = json.loads(receive_exact(connection, length))
                    print(f"WebSocket message type: {hello.get('type')}", flush=True)
                    if hello.get("type") != "hello":
                        raise ValueError("expected RV hello")
                    print("PASS: DNS, TCP, certificate-verified TLS, WebSocket upgrade, RV hello", flush=True)
                    return True
        except (OSError, ValueError, EOFError) as error:
            print(f"Failed: {error}", flush=True)
    return False

results = [probe(socket.AF_INET, "IPv4"), probe(socket.AF_INET6, "IPv6")]
print(f"\nResult: IPv4={'PASS' if results[0] else 'FAIL'}; IPv6={'PASS' if results[1] else 'FAIL'}")
sys.exit(0 if any(results) else 1)
