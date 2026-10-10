# Sanitized receive experiment evidence

Measurements recorded on 2026-10-10 using a FLEX-8600M, SmartSDR 4.2.18.41174,
ARM64 glibc 2.36 and Linux 5.15.92-v8+. The radio already had native IPv6
available. This summary omits addresses, serials, process identifiers, private
paths, access tokens, captures and audio recordings.

The retained library SHA256 was
`75c4cf13d407c037b430dd5d8639c6d0bb12a8598fba0c74e11afef7c16cd355`.
The native job checked that SmartSDR mapped that library alone, owned real
IPv6 TCP 4992 and UDP 4991/4993 sockets, and retained ordinary IPv4 discovery,
internal and SmartLink sockets. Activation used a current-boot systemd drop-in.

| Observation | Measured result |
| --- | --- |
| Bounded independent meter receive checks | 151 IPv6 and 150 IPv4 VITA meter packets, class 0x8002 |
| Aether output PCM window | 239,872 frames at 48 kHz, about 4.997 seconds |
| Panadapter statistics interval | 94.117 seconds |
| FFT frames | 24.82017 per second |
| Native waterfall updates | 12.49509 per second |
| Audio output underrun counter | 44 at start and 44 at the later snapshot |

The 44 underruns predated the comparison interval: no additional counted
underrun occurred between those snapshots. That counter comparison and short
PCM window do not prove glitch-free audio or long-term reliability. The
panadapter rates are interval statistics, not a packet-loss or latency proof.

The local fixtures separately verified ABI shape, replayed socket options,
concurrent IPv4/IPv6 traffic, multiple UDP ports and rejected lifecycles.
The original archival recovery tests exercised timer races and retry paths
with fake system state. Raw evidence remains outside this publication package.
No RV station/client session, iPhone client, WAN path, transmit operation,
reboot-persistent activation or other radio/firmware compatibility is asserted.
