# Operator-owned SSH diagnostic container

Original MIT network inspection recipe, authored by J.J. Boyd with Codex.
The Alpine ARM64 image contains SSH/SFTP, iproute2, DNS/TLS tools, tcpdump and
Python. It listens on TCP 22222 using public-key authentication only; it
generates its own SSH host key at startup and logs the fingerprint. Compare
that fingerprint when establishing operator-owned SSH trust.

From the package directory, prepare a fresh dedicated key outside the build
context, and copy only its public half into the ignored local input:

```sh
key_dir="$(mktemp -d)"
ssh-keygen -t ed25519 -f "$key_dir/probe" -C flex-diagnostic
cp "$key_dir/probe.pub" diagnostic-container/authorized_keys
chmod 600 diagnostic-container/authorized_keys
```

Keep the private key in operator-owned secure storage; it never belongs in
this package or image. `authorized_keys` is deliberately absent from Git.
The Dockerfile requires that user-provided file and fails without it.

For local inspect-only use, build a tagged image and publish SSH on loopback:

```sh
docker build --platform linux/arm64 -t flex-native-diagnostic:local diagnostic-container
docker run --rm -d --name flex-native-diagnostic \
  -p 127.0.0.1:22222:22222 flex-native-diagnostic:local
docker logs flex-native-diagnostic
ssh -o IdentitiesOnly=yes -i "$key_dir/probe" -p 22222 root@127.0.0.1 net-report
docker stop flex-native-diagnostic
```

Compare the logged host-key fingerprint before accepting SSH trust. On a Linux
Docker host, optional `--network host` replaces the port mapping and exposes
the SSH listener on the host's interfaces; it provides shared network inspection.
The recipe requires no host mounts, host PID mode, privileged mode or added
capabilities. Bridged inspection reports the container's own network namespace.

Build the waveform-shaped linux/arm64 OCI archive:

```sh
docker buildx build --platform linux/arm64 --provenance=false \
  --output type=oci,compression=gzip,dest=nereus-net-probe.oci.tar \
  diagnostic-container
gzip nereus-net-probe.oci.tar
```

Keep the compressed archive intact for an independent owner-managed installation
through the supported waveform workflow. This recipe follows the ARM64 OCI
shape and two labels described by the
[Flex waveform example](https://github.com/flexradio/waveform-example).
It adds no modulation mode, data proxy or native shim deployment automation.
After an independent owner-authorized installation, set `RADIO_ADDRESS` to its
verified reachable SSH endpoint and compare its startup host-key fingerprint:

```sh
ssh -o IdentitiesOnly=yes -i "$key_dir/probe" -p 22222 "root@$RADIO_ADDRESS" net-report
```

An advertised radio address may differ from a reachable processor endpoint;
this example does not establish routing or prove an on-radio installation.

An ordinary Docker bridge provides container networking. In the observed
waveform deployment the container shared the host network namespace, so
`net-report` could inspect host interfaces/routes/listeners. Root SSH here
means container root. It does not provide host-root filesystem access, host
PID access, systemd control, boot configuration or a native execution bridge.
Those were separate owner-authorized prerequisites of the radio experiment.

Inside the diagnostic shell, `net-report` inspects network configuration,
capabilities, DNS and IPv6 socket availability without changing them. Its
output includes local addresses and DNS details; sanitize before publication.
`rv-probe [hostname]` makes an external certificate-verified TLS/WebSocket
hello check, defaulting to the Nereus RV service. It does not register a station,
pair a device or create a client/media session. Exit zero means at least one
family passed; read both IPv4 and IPv6 outcomes.

Packet capture depends on granted capabilities; installing tcpdump grants none.
Do not publish collected reports/captures, generated keys or image archives in
this source package. The native shim uses glibc and is built separately from
this Alpine/musl image. Remove an independently installed diagnostic container
when the operator's investigation is finished.
