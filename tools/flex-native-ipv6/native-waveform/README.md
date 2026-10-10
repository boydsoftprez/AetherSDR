# Native descriptor helper waveform

These are the original MIT Go sources and tests for the native helper used in
the successful experiment, with its ARM launcher and legacy waveform manifest.
The Go standard-library distribution notice is retained in `LICENSE-Go`.
The manifest's field syntax follows the public
[FreeDV waveform example](https://github.com/n5ac/smartsdr-dsp/blob/20588edcd9fd02d8c296c740b8fc969adb3b54dd/DSP_API/Waveform/FreeDV/FreeDV.cfg).
Both setup and end sections contain no radio commands. No proprietary code was
copied; this helper registers no modulation mode and sends no RF commands.

Build from the package directory, choosing a new private directory outside the
Git checkout:

```sh
python3 native-waveform/build.py --output-dir "$HOME/flex-native-private"
```

The builder runs the five original Go tests in isolated Linux Docker, then
cross-compiles static ARM64 and ARMv7 binaries using `golang:1.25-bookworm`.
It generates a fresh 32-byte authentication token and embeds it at link time.
The private output includes `client-token.txt` and
`NereusNativeProbe.ssdr_waveform`. The package is a ZIP with a single
`NereusNativeProbe/` prefix, executable launchers/binaries and the license notices.
The token file is excluded from the ZIP, but the token is embedded in its
executables: keep the resulting package private too. No shared token or compiled
private package is committed to this branch. The build refuses output inside
the checkout and refuses to replace an existing output directory.

Install/restart this private legacy waveform only when the operator runner is
ready; the helper has a hard 60-second lifetime and exits after one authenticated
operation. The runner performs this upload through the public Flex file API.
TCP port 22223 serves either a bounded read-only metadata report or the explicit
authenticated `host-root-fd` operation. The latter opens the native `/` directory
and sends its descriptor over an abstract Unix socket to a root receiver inside
the SSH diagnostic container. The native helper and receiver check peer UIDs;
the measured native sender UID was 78666 and receiver UID was 0.

That deliberately delegated descriptor lets the operator receiver reach the
native Podman socket through `/proc/self/fd/…/run/podman/podman.sock`.
The [operator runner](../operator/README.md) uses that administrative socket to
launch a bounded temporary privileged host-PID/host-network job, then `nsenter`
into the host mount/network namespaces and execute native Python. Containers
carry administration; the native SmartSDR process carries radio data.

This is the measured access arrangement for the tested firmware, not a claim
that ordinary container SSH supplies host privileges on every radio. The source
contains the complete arrangement and preserves its token and UID checks.
The helper's root descriptor is read-only at the descriptor level; the receiver
is root and its later administrative operations are privileged. A read-only
descriptor does not make those operations read-only.

Local package-boundary tests:

```sh
python3 -m unittest discover -s native-waveform -p test_build.py -v
```

The five Go tests cover authentication, actual directory descriptor transfer,
single-operation lifetime, idle timeout, encoded report bounds and metadata.
They run on local container loopback and do not contact a radio.
