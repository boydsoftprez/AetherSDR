#!/usr/bin/env python3
"""Receive a deliberately delegated host directory FD; inspect boot/Podman only.

Runs in the operator's diagnostic container. An owner-only token is supplied on stdin as
the first line. No host filesystem writes, container creation or RF commands.
"""
# Copyright (c) 2026 J.J. Boyd. SPDX-License-Identifier: MIT
import array
import json
import os
import socket
import struct
import sys

token = sys.stdin.readline().strip()
if len(token) != 64 or any(c not in "0123456789abcdef" for c in token):
    raise RuntimeError("invalid receiver authentication configuration")
listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
listener.settimeout(75)
listener.bind("\0nereus-native-rootfd-" + token[:16])
listener.listen(1)
print("READY: read-only native root-directory descriptor receiver", flush=True)
received = []
try:
    connection, _ = listener.accept()
    with connection:
        peer_pid, peer_uid, peer_gid = struct.unpack("3i", connection.getsockopt(
            socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))
        if peer_uid != 78666:
            raise RuntimeError("unexpected native sender UID")
        connection.settimeout(5)
        data, ancillary, flags, _ = connection.recvmsg(1, socket.CMSG_SPACE(4 * 4))
        for level, kind, payload in ancillary:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                fds = array.array("i")
                fds.frombytes(payload[:len(payload) - (len(payload) % fds.itemsize)])
                received.extend(fds)
        if data != b"R" or len(received) != 1 or flags & socket.MSG_CTRUNC:
            raise RuntimeError("invalid directory descriptor transfer")
    root_fd = received[0]
    import http.client
    class UnixHTTP(http.client.HTTPConnection):
        def connect(self):
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect(f"/proc/self/fd/{root_fd}/run/podman/podman.sock")
    def request(method, path, body=None):
        client = UnixHTTP("localhost", timeout=25)
        encoded = json.dumps(body).encode() if body is not None else None
        client.request(method, path, encoded, {"Content-Type": "application/json"})
        response = client.getresponse()
        payload = response.read(16*1024*1024)
        status = response.status
        client.close()
        if status >= 300:
            raise RuntimeError(f"API {method} {path}: {status} {payload.decode(errors='replace')}")
        if "/archive?" in path:return payload
        if "/logs?" in path:
            decoded = bytearray()
            cursor=0
            while cursor+8 <= len(payload):
                length=int.from_bytes(payload[cursor+4:cursor+8],"big")
                end=cursor+8+length
                if end>len(payload):raise RuntimeError("truncated probe logs")
                decoded.extend(payload[cursor+8:end]);cursor=end
            if cursor!=len(payload):raise RuntimeError("trailing probe log bytes")
            return decoded.decode("utf-8")
        try: return json.loads(payload)
        except ValueError: return payload.decode(errors="replace")
