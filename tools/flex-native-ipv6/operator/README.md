# Owner-operated native transport

Original MIT Python sources by J.J. Boyd, authored with Codex. This directory
packages the transport actually used by the successful native IPv6 experiment.
It includes the authenticated descriptor receiver, native job tail, public Flex
waveform uploader, receive-only control/meter probe, and administrative ACK.
The executable transport and helper protocol are retained; personal addresses,
key paths, token paths and experiment-output directories are operator inputs.

The complete administrative path is:

1. Owner SSH authenticates to the installed diagnostic container on TCP 22222.
   `native-run.py` copies the job's input files and generated receiver there.
2. The receiver reads a fresh 64-character lowercase hex token from **stdin**,
   listens on the token-specific abstract AF_UNIX address, and announces READY.
3. The runner uploads the freshly built private native waveform through the
   public FlexLib file API. It then authenticates to that helper on TCP 22223
   and explicitly requests `host-root-fd`.
4. The native helper opens `/` and delegates that directory FD with SCM_RIGHTS.
   The receiver requires exactly one FD, the `R` marker and native sender UID
   78666; the native helper requires receiver UID 0. These are the measured
   firmware identities, not portable assumptions about every radio.
5. The receiver follows `/proc/self/fd/<fd>/run/podman/podman.sock`, inspects the
   existing `nereus-net-probe` container image, and creates a temporary root
   job with `Privileged=True`, host PID mode and host network mode. Its entry
   point is `/usr/bin/nsenter -t 1 -m -n -- /usr/bin/python3 -c ...`, so Python
   executes in the native host mount/network namespaces.
6. The job receives `WORKDIR`, copied input files and optional `EXPECTED_SHA256`.
   The receiver polls it for at most 600 seconds, requests KILL on timeout,
   saves the exit status and logs, deletes the temporary container, and removes
   ordinary temporary host files. The runner removes copied container files
   and reaps its local SSH/upload children, including on orchestration failure.

Ordinary container root SSH alone supplies none of the delegated host-root FD,
host Podman authorization or native execution privileges above. The installed
container must use the tested shared network arrangement and be named
`nereus-net-probe`; its image must contain `/usr/bin/nsenter`. The native host
must provide the observed Podman socket and `/usr/bin/python3`. See the
[diagnostic container](../diagnostic-container/README.md) and
[native waveform builder](../native-waveform/README.md) for their complete inputs.
The independent installation and SSH host-key fingerprint verification must be
completed by the owner before invoking this transport.

`native-rootfd-prefix.py.inc` is intentionally incomplete Python: it ends
inside the original `try` and is assembled with `native-job-tail.py.inc` by the
runner. The historical prefix's read-only docstring/READY line describes the
original inspection proof. The assembled receiver performs the privileged
writes and native job described above. Only that relevant prefix was retained;
the larger preload-proof experiment is not a runtime dependency.

Build a fresh native package into a new private directory outside the checkout.
The token is embedded in its executables, so **both the token file and waveform
ZIP must remain private**. The runner and uploader reject public package modes;
the runner also rejects a public token or state directory. It never places the
token in SSH/upload argv, generated receiver source or diagnostic output. The
operator must supply the matching token/package pair from the same fresh build;
a file-mode/format check cannot establish that pairing.

From `tools/flex-native-ipv6`, set explicit owner inputs. `RADIO_HOST` is the
reachable processor endpoint for SSH and helper/upload; `RADIO_IPV6` is the
native IPv6 address exercised by the external client. Paths below are examples
to replace, not shipped credentials or a historic private package:

```sh
RADIO_HOST='owner-radio.example'
RADIO_IPV6='2001:db8::1234'
SSH_KEY='/absolute/operator-private/probe'
KNOWN_HOSTS='/absolute/operator-private/verified-known-hosts'
PRIVATE_BUILD='/absolute/operator-private/fresh-native-build'
STATE_DIR='/absolute/operator-private/native-run-state'
SHIM='/absolute/operator-build/libnereus_ipv6.so'
SHIM_SHA256='replace-with-reviewed-64-character-lowercase-sha256'

python3 native-waveform/build.py --output-dir "$PRIVATE_BUILD"
mkdir -p "$STATE_DIR"
chmod 700 "$STATE_DIR"

native_args=(
  --host "$RADIO_HOST" --ssh-key "$SSH_KEY" --known-hosts "$KNOWN_HOSTS"
  --token-file "$PRIVATE_BUILD/client-token.txt"
  --native-package "$PRIVATE_BUILD/NereusNativeProbe.ssdr_waveform"
  --state-dir "$STATE_DIR" --expected-sha256 "$SHIM_SHA256"
)
```

Build the shim for the measured native ARM64/glibc target using its
[build instructions](../shim/README.md), then independently review its SHA-256
and supply that value. The runnable acceptance/revert jobs require the explicit
hash; a historical digest never stands in for an independently rebuilt file.
For an arbitrary bounded original job, run `python3 operator/native-run.py
JOB.py OUTPUT.json "${native_args[@]}" --file INPUT` (repeat `--file` as needed).
The input basenames become `WORKDIR/<basename>` on the host; duplicate basenames
are rejected. Jobs must remove any subdirectories and their own service changes.

For receive/control acceptance on the already IPv6-enabled tested host, start
acceptance in one terminal. It makes a **guarded SmartSDR service restart**, arms
an independent rollback timer and waits for administrative ACK. It does not
reboot the radio or key RF:

```sh
python3 operator/native-run.py deployment/host-smartsdr-acceptance.py \
  "$STATE_DIR/acceptance.json" "${native_args[@]}" --file "$SHIM"
```

While that command is pending, use a second terminal with the same owner inputs.
The job's logs are fetched after exit, so `acceptance_ready` is not streamed into
the local report while ACK is pending. Allow activation to finish; the ACK socket
listens only after the native process/socket checks pass. Run the external probe
once IPv6 control is reachable and inspect its successful meter-class UDP result.
The probe retries for 18 seconds and fails if real meter packets are absent:

```sh
python3 operator/control-meter-client.py --host "$RADIO_IPV6" \
  > "$STATE_DIR/control-meter-ipv6.json"
```

After the external receive/control result meets the operator's acceptance
criteria, send ACK **through the existing owner SSH session to host-network
loopback**. Run within the acceptance job's 180-second ACK window:

```sh
python3 operator/ack-accepted-runtime.py \
  --host "$RADIO_HOST" --ssh-key "$SSH_KEY" --known-hosts "$KNOWN_HOSTS" \
  --output "$STATE_DIR/acceptance-ack.json"
```

That script runs the original `{"accept":true}` message against
`127.0.0.1:46861` remotely and requires `{"acknowledged":true}`. No port forwarding
or public bind of administrative port 46861 is part of this procedure. If the
listener is not ready, the command fails; do not ACK a failed external probe.
Without timely ACK, acceptance restores the original service and the independent
rollback timer remains its backup. The completed acceptance report must show
`retained: true`, the reviewed hash and verified cleanup/rollback disposition.

To revert retained acceptance through the same transport, use the same reviewed
hash; revert checks the retained stable library before removing its artifacts:

```sh
python3 operator/native-run.py deployment/host-smartsdr-revert.py \
  "$STATE_DIR/revert.json" "${native_args[@]}"
```

Native IPv6 boot/network jobs are separately available in
[deployment/](../deployment/README.md). They are not implicit prerequisites that
this runner silently changes; inspect their strict firmware checks and operate
them explicitly only when needed. The uploader's original `--restart` restarts
only `NereusNativeProbe`; `--uninstall` removes that diagnostic waveform. Neither
is a radio reboot command, and the runner only requests package upload.

Cleanup retains the original failure limits: if the receiver dies or loses the
Podman API, its polling deadline cannot execute; if container deletion raises,
subsequent host-directory removal is skipped. A job must arrange its own durable
recovery for persistent changes. Acceptance/revert preserve the independent
systemd rollback design, which is distinct from the receiver's temporary-job
polling. Keep failed run evidence and use the owner recovery path; a timeout is
not evidence that every host artifact was removed.

All local tests fake SSH/SCP, waveform upload and native-helper networking; they
do not contact a radio, read a real token, change services or transmit RF:

```sh
python3 -m unittest discover -s operator -p test_operator.py -v
```

The uploader preserves the original official FlexLib 4.2.20 `Radio.cs`
references at lines 2301, 12966–12967 and 8554. The control probe preserves its
public Aether VITA-header and official TCP client API notes. These are original
client glue and operator transport, licensed under the adjacent MIT license.
