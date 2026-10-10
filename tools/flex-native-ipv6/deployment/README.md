# Native host deployment jobs

Original MIT host jobs, authored by J.J. Boyd with Codex. These expose the
tested native acceptance/recovery shape with an explicit digest for an
independently reviewed build. They contain no SSH, credentials, waveform
access bootstrap, packet proxy or radio/client/RF commands.

The native executor supplies `WORKDIR` and `EXPECTED_SHA256` globals. Acceptance
requires `WORKDIR/libnereus_ipv6.so`; revert checks the retained host library.
Both refuse a missing or malformed digest before writes and use that explicit
64-character lowercase SHA256 throughout activation, recovery and cleanup.
The fixed historical artifact identity remains in [host-reference/](../host-reference/README.md);
these jobs do not silently trust that old digest for a new build.

## Build and fake-system verification

From the package directory, run source-only deployment tests in scratch storage:

```sh
docker run --rm --platform linux/arm64 --network none --read-only --cap-drop ALL \
  --tmpfs /tmp:rw,exec,nosuid,size=64m --tmpfs /work:rw,exec,nosuid,size=64m \
  --mount "type=bind,src=$PWD,dst=/source,readonly" -w /work gcc:13 sh -c \
  'cp -R /source/. /work/ && make -C shim libnereus_ipv6.so && cd deployment && python3 -B -m unittest discover -v'
```

The 12 original acceptance and one revert fake-system cases now inject the
freshly built fixture's digest and retain their recovery assertions. Additional
tests exercise missing/malformed/mismatched digests, file-job execution and
optimized-Python rejection. They use temporary paths, fake systemctl and
isolated loopback; they perform no host/radio administration. This command
does not run the shim's interface-alias fixture and needs no NET_ADMIN.

To export a separately reviewed Linux ARM64/glibc library without writing into Git:

```sh
artifact_dir="$(mktemp -d)"
docker run --rm --platform linux/arm64 --network none --read-only --cap-drop ALL \
  --tmpfs /tmp:rw,noexec,nosuid,size=64m --tmpfs /work:rw,exec,nosuid,size=64m \
  --mount "type=bind,src=$PWD,dst=/source,readonly" \
  --mount "type=bind,src=$artifact_dir,dst=/artifacts" -w /work gcc:13 sh -c \
  'cp -R /source/. /work/ && make -C shim libnereus_ipv6.so && cp shim/libnereus_ipv6.so /artifacts/'
reviewed_sha256="$(shasum -a 256 "$artifact_dir/libnereus_ipv6.so" | cut -d ' ' -f 1)"
```

Review the source, binary architecture and target glibc compatibility before
using that artifact. Retain its digest independently; a checksum identifies
the selected build and does not establish trust in unreviewed code.

## Acceptance and guarded revert

These commands run **on the already owner-authorized native Linux host**, from
the staged package directory, as host root. Set `native_workdir` to the absolute
staging directory containing the selected library and `reviewed_sha256` to its
independently retained digest. Workstation artifact paths are not host paths.
The separate operator transport can execute the same Python files by injecting
`WORKDIR` and `EXPECTED_SHA256`; it must upload the selected library for acceptance.

```sh
python3 deployment/native-host-job.py --job accept \
  --workdir "$native_workdir" --expected-sha256 "$reviewed_sha256"
```

Acceptance requires native IPv6, systemd, host `/proc`, `smartsdr.service`,
`inhibit-power-pic.service` and the observed socket contract. It verifies an
independent 300-second rollback timer before writing a current-boot `/run`
drop-in and restarting the guarded service. Selected sockets are TCP 4992 and
UDP 4991/4993; discovery/internal/SmartLink sockets must stay ordinary IPv4.
The original power-PIC guard is required: an earlier unguarded restart rebooted
the radio. Failed activation or acceptance attempts recovery and a timer retry.

After `acceptance_ready`, evaluate the Aether receive/control result separately.
Only then, from another authorized native-host session within 180 seconds:

```sh
python3 - <<'PY'
import json, socket
with socket.create_connection(('127.0.0.1', 46861), timeout=3) as channel:
    channel.sendall(b'{"accept":true}\n')
    assert json.loads(channel.recv(512)) == {'acknowledged': True}
PY
```

The local ACK protocol has no authentication of its own; keep it within the
owner-controlled host context, without a published port. Successful acceptance
retains the exact library and `/run` drop-in for this boot; reboot does not
reactivate the shim. Retention still requires final process/socket/hash checks
and settled rollback state. A `root` API parameter supports fake filesystem
tests; it does not sandbox real systemctl calls.

To revert that exact retained library, preserving the same digest:

```sh
python3 deployment/native-host-job.py --job revert \
  --workdir "$native_workdir" --expected-sha256 "$reviewed_sha256"
```

Revert verifies exact ownership, retries reload/guard/restart, checks the same
boot and confirms no native process maps the shim before unlinking it. A
mismatch stops recovery changes; inspect ownership and the reported state.
This reverses shim activation, not host IPv6 configuration.

## Separate firmware-specific host IPv6 jobs

The three `native-ipv6-*.py` scripts preserve the measured boot/network inputs,
internal FPGA address/MAC constants and dated backup filenames. They refuse
different inputs or existing owned outputs through the original assertions.
Python assertions must remain enabled; both these files and the local runner
reject optimized execution. They are not a general installer for other radios.

Each job is a distinct explicit owner action through the same local CLI:

```sh
python3 deployment/native-host-job.py --job boot-edit \
  --workdir "$native_workdir" --expected-sha256 "$reviewed_sha256"
python3 deployment/native-host-job.py --job network-enable \
  --workdir "$native_workdir" --expected-sha256 "$reviewed_sha256"
python3 deployment/native-host-job.py --job host-rollback \
  --workdir "$native_workdir" --expected-sha256 "$reviewed_sha256"
```

These examples are alternatives/ordered stages, not a script to run together.
`boot-edit` changes the exact `/boot/cmdline.txt` IPv6 disable argument and
retains its original backup; a separate owner-planned reboot is required before
kernel IPv6 becomes available. `network-enable` runs only after that prerequisite
and adds the observed Ethernet/loopback sysctl and networkd overrides while
retaining the original IPv4 configuration. It may interrupt connectivity and
has no transaction across all its writes; a failure can leave partial state.
Inspect that state rather than blindly repeating or relaxing its assertions.

After guarded shim revert, `host-rollback` requires the exact original backup
and owned overrides, restores the disabled boot argument and removes those
overrides. It reports a required reboot but does not reboot or disable live
interfaces. The publication verification does not execute these host-changing
jobs. The required library digest does not authenticate their configurations;
their exact content checks do. No host boot edit, reboot or network enable is
automatically performed by acceptance or by the local runner.

`--file /owner-reviewed/job.py` can replace `--job` for a trusted native job,
with the same required workdir/digest globals. There is no access escalation
in this entry point: ordinary diagnostic container SSH does not provide the
native host scope. Review [the measured limits](../EVIDENCE.md) before any
independent deployment; no TX, RV, iPhone, WAN or long-term reliability claim
is added by making these sources runnable.
