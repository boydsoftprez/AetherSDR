# Isolated socket shim fixtures

This original MIT library uses public Linux/glibc interfaces. It replaces an
explicitly selected fresh AF_INET wildcard-bound socket with a dual-stack
AF_INET6 socket at bind, using `dup3` to retain the descriptor in the same PID.
Application-facing peer aliases represent IPv6 address plus scope, while real
transport ports remain independent. Real IPv4 peers retain their IPv4 address.
The synthetic `198.18.0.0/15` range is never used as a network route.

Sources require Linux/glibc, GCC, make, libdl and pthreads; clients use Python 3
standard-library modules. They are not a macOS dylib or an Alpine/musl build.
The ARM64 fixture build used `gcc:13` with `-O2 -g -Wall -Wextra -Werror`.

From this directory, run the full suite in isolated scratch storage:

```sh
docker run --rm --platform linux/arm64 --network none --read-only \
  --cap-drop ALL --cap-add NET_ADMIN \
  --tmpfs /tmp:rw,noexec,nosuid,size=64m \
  --tmpfs /work:rw,exec,nosuid,size=64m \
  --mount "type=bind,src=$PWD,dst=/source,readonly" \
  -w /work gcc:13 sh -c 'cp -R /source/. /work/ && make test'
```

The source mount stays read-only; compilation and tests run in temporary
storage. Only the isolated container loopback carries traffic, including `::1`.
NET_ADMIN permits `mixed_client.py` to assign `172.30.1.2/32` as a loopback
test stand-in. **Run `make test` only inside the isolated test container:** that fixture
changes an interface alias and does not remove it. No radio is used.

The suite checks six ordinary and six accept-before-fork TCP/UDP sessions;
mixed concurrent IPv4/IPv6 peers on TCP 4992 and UDP 4991/4993; socket options,
flags, poll/select, address truncation and error propagation; multiple UDP
ports per peer; and unsupported descriptor lifecycle rejection. Expected
`EOPNOTSUPP` diagnostics are part of negative tests. `PASS full shim suite`
marks successful completion. Compiled artifacts disappear with the container.

`NEREUS_IPV6_UDP_PORTS` accepts 1–32 comma-separated numeric ports in
1..65535. A present plural value overrides `NEREUS_IPV6_UDP_PORT`; malformed
or empty plural input disables all UDP conversion. Unset/invalid TCP selection
leaves TCP unchanged. The old single UDP variable works when plural is absent.

A selected bind must occur on a fresh, unshared, single-owner socket before
epoll registration or sharing with threads/processes. Only the documented
pre-bind options and ordinary flags are replayed. Unsupported pre-bind dup,
fork, asynchronous ownership, epoll and socket options fail explicitly.
Bind first, then share/register or adjust other options.

Direct syscalls, hidden libc calls, io_uring, close_range, SCM_RIGHTS, arbitrary
ioctl state and ownership/close races bypass this bounded contract. DNS,
discovery, multicast, packet-info ancillary translation and arbitrary outbound
or connected UDP lifecycles are not covered. Converted descriptors must be
below 65536; the stable peer table holds 1024 address/scope entries and fails
with ENOSPC when full. Converted sockets reject real IPv4 peers in the alias
range. Mutexes protect metadata, not blocking network operations.

The observed SmartSDR selections and live results are documented in the
[package README](../README.md) and [evidence summary](../EVIDENCE.md).
Fixture success alone is not native radio deployment evidence.
