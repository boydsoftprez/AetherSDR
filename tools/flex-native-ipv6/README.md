# Native Flex IPv6 experiment

Original clean-room MIT sources by J.J. Boyd, authored with Codex. This package
preserves the bounded Linux socket experiment used with the IPv6 AetherSDR
client. It contains no vendor implementation, private ABI, radio firmware,
credentials, recordings or deployment bootstrap.

The tested flow was AetherSDR over native IPv6 TCP control and UDP receive
streams to the radio's SmartSDR process. An explicitly selected `LD_PRELOAD`
library replaces selected sockets inside that same process with real kernel
dual-stack IPv6 sockets. The application still sees IPv4-shaped addresses;
stable synthetic peer aliases remain inside the process. There is no data
proxy, container packet forwarding or address translation on the wire.

| Directory | Purpose |
| --- | --- |
| [shim/](shim/README.md) | Original public Linux/glibc socket shim and isolated local fixtures |
| [diagnostic-container/](diagnostic-container/README.md) | ARM64 SSH network inspection recipe; operator supplies a fresh public key |
| [host-reference/](host-reference/README.md) | Archived host acceptance/revert jobs and fake-system tests |

For the measured SmartSDR socket setup, selection was:

```sh
NEREUS_IPV6_TCP_PORT=4992
NEREUS_IPV6_UDP_PORTS=4991,4993
LD_PRELOAD=/operator-owned/path/libnereus_ipv6.so
```

These are environment values documenting the observed host experiment, not an
installation command. Only AF_INET wildcard binds matching the configured
socket type and port convert. UDP discovery 4992, internal UDP 30000/30001,
TCP 4994, specific-address binds and unrelated outbound sockets stay untouched.
Pre-bind SO_REUSEADDR, TCP_NODELAY and the successful SO_RCVBUF request are
preserved, alongside ordinary descriptor/nonblocking flags.

The live evidence came from a FLEX-8600M running SmartSDR 4.2.18.41174,
native ARM64 glibc 2.36 and Linux 5.15.92-v8+. The host already had native
IPv6 enabled and an owner-authorized administrative execution path. Neither
the shim nor the diagnostic container establishes those prerequisites.
See [EVIDENCE.md](EVIDENCE.md) for sanitized measurements and their limits.

Acceptance retained a library and a systemd drop-in under `/run`, so activation
lasted for the current boot. Guarded service restarts first started the installed
vendor `inhibit-power-pic.service`. An earlier unguarded restart caused a full
radio reboot. The archived jobs verify their own exact artifact, active native
process/socket state, an independent rollback timer, and guarded recovery.
They are host-specific references rather than a turnkey deployment method.

Run the shim fixtures only with the isolated procedure in its README. The full
suite assigns a test loopback alias and needs NET_ADMIN inside that isolated
network namespace. The diagnostic SSH recipe is separate: ordinary bridged
Docker networking remains container-scoped; the observed waveform deployment
shared the host network namespace. Shared network visibility and root inside
the container do not imply host filesystem, process or administrative access.

The evidence supports a bounded native IPv6 receive/control experiment. It
does not establish RV, iPhone clients, WAN routing, transmit operation,
long-term reliability, or support for other firmware and radio models. Review
the shim lifecycle limits before considering any independent host experiment.

Each subdirectory retains its MIT license. The shim and archival reference
sources are preserved unchanged; diagnostic files add explicit original MIT
notices. Public socket interfaces and observed behavior informed the shim.
The [Flex waveform example](https://github.com/flexradio/waveform-example)
informed packaging labels and ARM64 OCI shape only; no vendor code was copied.
