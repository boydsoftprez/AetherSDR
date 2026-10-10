#!/usr/bin/env python3
"""Bounded legacy diagnostic upload using the public FlexLib file API.

Protocol basis: official FlexLib 4.2.20 Radio.cs:2301,12966-12967,8554.
Original client glue. No firmware update, radio reboot, or RF commands.
"""
# Copyright (c) 2026 J.J. Boyd. SPDX-License-Identifier: MIT
import argparse
import pathlib
import socket
import stat
import time
import zipfile


class Control:
    def __init__(self, host):
        self.socket = socket.create_connection((host, 4992), 5)
        self.socket.settimeout(0.5)
        self.buffer = b""
        self.sequence = 0

    def lines(self):
        try:
            data = self.socket.recv(16384)
        except socket.timeout:
            return []
        if not data:
            raise RuntimeError("radio control connection closed")
        self.buffer += data
        if len(self.buffer) > 65536:
            raise RuntimeError("radio control line exceeds bound")
        lines = []
        while b"\n" in self.buffer:
            raw, self.buffer = self.buffer.split(b"\n", 1)
            line = raw.decode("utf-8", errors="replace").strip()
            if line.startswith(("V", "H", "R")) or "|waveform " in line:
                print(line, flush=True)
            lines.append(line)
        return lines

    def command(self, command):
        self.sequence += 1
        sequence = self.sequence
        self.socket.sendall(f"C{sequence}|{command}\n".encode("ascii"))
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            for line in self.lines():
                if line.startswith(f"R{sequence}|"):
                    fields = line.split("|", 2)
                    if int(fields[1], 16) != 0:
                        raise RuntimeError(f"command rejected: {line}")
                    return fields[2] if len(fields) > 2 else ""
        raise RuntimeError("radio command reply timed out")

    def observe(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.lines()


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--package", type=pathlib.Path)
    parser.add_argument("--restart", action="store_true")
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args(argv)
    if sum((args.package is not None, args.restart, args.uninstall)) != 1:
        parser.error("choose exactly one diagnostic operation")
    payload = None
    if args.package is not None:
        if not args.package.is_file() or args.package.is_symlink() or stat.S_IMODE(args.package.stat().st_mode) & 0o077:
            raise ValueError('native token-bearing package must be a private regular file (chmod 600)')
        if args.package.name != "NereusNativeProbe.ssdr_waveform":
            raise RuntimeError("only the named diagnostic package is allowed")
        payload = args.package.read_bytes()
        if not payload or len(payload) > 8 * 1024 * 1024:
            raise RuntimeError("package outside diagnostic size bound")
        with zipfile.ZipFile(args.package) as archive:
            for name in archive.namelist():
                path = pathlib.PurePosixPath(name)
                if path.is_absolute() or ".." in path.parts or "\\" in name:
                    raise RuntimeError("unsafe package path")
            cfg_names = [n for n in archive.namelist() if n.endswith(".cfg")]
            if len(cfg_names) != 1:
                raise RuntimeError("expected one diagnostic manifest")
            manifest = archive.read(cfg_names[0]).decode("utf-8")
            setup = manifest.lower().split("[setup]", 1)[1].split("[end]", 1)[0]
            if any(line.strip() and not line.lstrip().startswith(("#", ";"))
                   for line in setup.splitlines()):
                raise RuntimeError("diagnostic setup must contain no radio commands")
    control = Control(args.host)
    try:
        control.command("sub waveform all")
        if payload is not None:
            control.command("file filename NereusNativeProbe.ssdr_waveform")
            port_text = control.command(f"file upload {len(payload)} new_waveform")
            port = int(port_text.strip())
            if not 1024 <= port <= 65535:
                raise RuntimeError("invalid negotiated upload port")
            time.sleep(0.3)
            with socket.create_connection((args.host, port), 5) as upload:
                upload.settimeout(20)
                upload.sendall(payload)
            print(f"Sent {len(payload)} diagnostic package bytes to port {port}.", flush=True)
            control.observe(15)
        elif args.restart:
            control.command("waveform restart NereusNativeProbe")
            control.observe(4)
        else:
            control.command("waveform uninstall NereusNativeProbe")
            control.observe(4)
    finally:
        control.socket.close()


if __name__ == "__main__":
    main()
