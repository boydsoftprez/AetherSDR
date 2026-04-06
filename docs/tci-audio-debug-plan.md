# Debug & Fix: TCI Audio with WSJT-X (Discussion #762)

## Context

**Discussion**: [ten9876/AetherSDR#762](https://github.com/ten9876/AetherSDR/discussions/762)
Multiple users report TCI audio broken on all platforms (Win11/macOS/Linux) in v0.8.4:
- **RX**: Signals on WSJT-X waterfall but FT8 does **not decode**
- **TX**: PTT keys radio but **no audio/RF** output
- Confirmed by K5WH (Win), LB2EG (all platforms), K6JS (Win11)

**Related open issues**: #841 (macOS TCI audio distorted + rigctld broken)

---

## Evidence Collected

### TCI Protocol Spec (ExpertSDR3 v2.0)
- Source: `ExpertSDR3/TCI` on GitHub, PDF spec v2.0 dated Jan 2024
- Binary header: 16 x uint32 = 64 bytes, C struct layout (native endian)
- Valid audio sample rates: 8/12/24/48 kHz
- Default format: float32, default channels: 2
- `length` field = number of real samples in data[] array
- **TX audio source**: `TRX:n,true,tci;` tells server to use TCI audio stream for TX. Without `,tci`, the mic signal is used
- **TX_CHRONO**: Server sends TX_CHRONO (type=3) timing frames to synchronize client TX audio delivery

### WSJT-X Improved TCI Client (`salyh/wsjtx-improved`)
- Files: `Transceiver/TCITransceiver.hpp` (header struct) and `TCITransceiver.cpp` (~1900 lines)
- **Hardcodes `audioSampleRate = 48000`** (line 216)
- **Does NOT send `audio_samplerate:48000;` to server** — expects server to handle it
- Sends `audio_start:0;` to enable RX audio
- RX: reads `pStream->data` float32 samples, applies 4x downsampling for FT8 decoder
- TX: sends float32 stereo at 48000 Hz, type=TX_AUDIO_STREAM(2)
- PTT: detects device name. For `"ExpertSDR3"` → sends `trx:0,true,tci;`. For anything else → sends `trx:0,true;` (no tci arg)
- Header struct has `reserv[9]` (9 reserved fields) vs AetherSDR's `reserved[8]` + separate `channels` field — **potential struct mismatch**

### AetherSDR TCI Server (`src/core/TciServer.cpp`, `TciProtocol.cpp`)
- Init burst sends `device:FLEX-8600 FLEX-8600;` (NOT "ExpertSDR3")
- Init burst sends `audio_samplerate:24000;`
- Default ClientState: rate=24000, channels=2, format=3(float32)
- Handles `audio_samplerate:` from client → creates Resampler if rate != 24000
- `onRxAudioReady()` (line 382): **Per-client loop with resampler, format, channels**
- `onDaxAudioReady()` (line 476): **Hard-codes 24kHz, single frame, no per-client handling**
- Both wired from MainWindow (lines 1972-1975): `audioDataReady → onRxAudioReady` and `daxAudioReady → onDaxAudioReady`
- `cmdTrx()` (TciProtocol.cpp:331): **Ignores 3rd arg** — never reads `,tci`
- TX binary handler: converts to float32 stereo, calls `feedDaxTxAudio()` with no guards
- **No TX_CHRONO support** — never sends TX_CHRONO frames to clients
- Header struct: `channels` is field 7 (byte offset 28), `reserved[8]` fills 32-63

### AudioEngine TX Gate (`src/core/AudioEngine.cpp:1197-1224`)
- Low-latency route (default): gates on `m_radioTransmitting` (line 1205)
- Radio-native route: gates on `m_transmitting && !m_daxTxMode` (line 1224)

---

## Open Questions (Must Verify Through Debugging)

| # | Question | How to verify |
|---|----------|---------------|
| Q1 | Does WSJT-X send `audio_samplerate:48000;` despite code analysis suggesting it doesn't? | WebSocket text log |
| Q2 | Which audio path fires — `onRxAudioReady`, `onDaxAudioReady`, or both? | Add counters/logging to both |
| Q3 | What `sampleRate` value does WSJT-X receive in binary frame headers? 24000? | Binary frame header dump |
| Q4 | Does WSJT-X header struct (`reserv[9]`) match AetherSDR's (`channels` + `reserved[8]`)? | Compare struct layouts byte-by-byte |
| Q5 | When WSJT-X sends TX audio, what are `m_radioTransmitting` and `m_transmitting` at that moment? | Log both flags in `feedDaxTxAudio()` |
| Q6 | Is `m_txStreamId` set (non-zero) when TCI is the only audio path? | Log stream ID in feedDaxTxAudio |
| Q7 | Does WSJT-X expect TX_CHRONO before sending TX audio, or does it send proactively? | WebSocket binary frame capture during TX |

---

## Phase 1: Diagnostic Instrumentation

Add targeted logging to capture the actual data flow. **All changes in `TciServer.cpp` and `AudioEngine.cpp`.**

### 1A. TCI Text Command Logger (`TciServer::onTextMessage`, line 177)

Add at the start of the command loop:
```cpp
qCInfo(lcCat) << "TCI <--" << ws->peerAddress().toString() << trimmed;
```

This captures every command WSJT-X sends, answering **Q1** (whether it sends `audio_samplerate:48000;`).

### 1B. RX Audio Path Counters (`onRxAudioReady` + `onDaxAudioReady`)

In `onRxAudioReady` (line 382), add a periodic counter:
```cpp
static int rxMainCount = 0;
if (++rxMainCount % 500 == 1)
    qCInfo(lcCat) << "TCI: onRxAudioReady fired" << rxMainCount << "times,"
                  << pcm.size() << "bytes," << m_clients.size() << "clients";
```

In `onDaxAudioReady` (line 476), same pattern:
```cpp
static int rxDaxCount = 0;
if (++rxDaxCount % 500 == 1)
    qCInfo(lcCat) << "TCI: onDaxAudioReady fired" << rxDaxCount << "times,"
                  << "channel=" << channel << pcm.size() << "bytes";
```

This answers **Q2** — which path fires and how often.

### 1C. RX Binary Frame Header Dump (`onDaxAudioReady`, after buildAudioFrame)

Log the first frame header sent to each client:
```cpp
static bool firstFrame = true;
if (firstFrame) {
    firstFrame = false;
    qCInfo(lcCat) << "TCI: first RX frame header:"
                  << "receiver=" << trx
                  << "sampleRate=" << 24000  // or cs.audioSampleRate after fix
                  << "format=3 channels=2"
                  << "length=" << stereoFrames
                  << "frameBytes=" << frame.size();
}
```

This answers **Q3**.

### 1D. TX Audio Flow Logger (`AudioEngine::feedDaxTxAudio`, line ~1197)

At the top of `feedDaxTxAudio()`, before any gating:
```cpp
static int txCallCount = 0;
if (++txCallCount % 200 == 1)
    qCInfo(lcAudio) << "feedDaxTxAudio called:" << txCallCount
                    << "radioTx=" << m_radioTransmitting
                    << "tx=" << m_transmitting
                    << "daxTxMode=" << m_daxTxMode
                    << "radioRoute=" << m_daxTxUseRadioRoute
                    << "txStreamId=" << m_txStreamId
                    << "bytes=" << pcm.size();
```

This answers **Q5** and **Q6**.

### 1E. TCI Binary TX Frame Logger (`TciServer::onBinaryMessage`, line 310)

After parsing the header:
```cpp
qCInfo(lcCat) << "TCI: RX'd TX binary frame:"
              << "receiver=" << hdr.receiver
              << "rate=" << hdr.sampleRate
              << "format=" << hdr.format
              << "channels=" << hdr.channels
              << "type=" << hdr.type
              << "length=" << hdr.length
              << "payloadBytes=" << payloadBytes;
```

This answers **Q7** and validates the frame format.

---

## Phase 2: Test Protocol with WSJT-X Improved

### Prerequisites
- AetherSDR debug build with logging from Phase 1
- WSJT-X Improved installed, configured for TCI
- FlexRadio connected, slice in DIGU mode on an active FT8 frequency (e.g. 14.074 MHz)
- Terminal visible to watch AetherSDR log output

### Test A: RX Audio Path

1. Start AetherSDR, connect to radio, ensure DIGU on 14.074 MHz
2. Enable TCI server in DIGI applet (port 50001)
3. Start WSJT-X Improved:
   - Settings → Radio: Rig = `TCI`, Server = `localhost:50001`
   - Settings → Audio: TCI Audio (both input and output)
4. Watch AetherSDR log for:
   - `TCI <-- audio_start:0;` (confirm audio started)
   - `TCI <-- audio_samplerate:...` (check if WSJT-X negotiates rate — **Q1**)
   - `onRxAudioReady` and/or `onDaxAudioReady` counters (**Q2**)
   - First RX frame header dump (**Q3**)
5. Observe WSJT-X waterfall:
   - Do signals appear? (confirms audio arrives)
   - Do FT8 signals decode? (confirms correct format/rate)
6. **Record findings** for each Q.

### Test B: TX Audio Path

1. Same setup as Test A
2. In WSJT-X, click **Tune** button
3. Watch AetherSDR log for:
   - `TCI <-- trx:0,true;` or `trx:0,true,tci;` (**Q4** — does it send `,tci`?)
   - Binary TX frame details from 1E (**Q7**)
   - `feedDaxTxAudio` log entries (**Q5**, **Q6**)
4. Check radio:
   - Does PTT engage? (keying works)
   - Does power meter show output? (audio reaches TX chain)
5. If no RF output, check:
   - Is `m_radioTransmitting` true when TX audio arrives?
   - Is `m_txStreamId` non-zero?
   - Is `m_daxTxUseRadioRoute` true or false?
6. **Record findings** for each Q.

### Test C: Struct Layout Verification

The WSJT-X header has `reserv[9]` (9 uint32 reserved = 36 bytes) while AetherSDR has `channels` (1 uint32) + `reserved[8]` (8 uint32 = 32 bytes + 4 bytes = 36 bytes). Offset check:

| Field | AetherSDR offset | WSJT-X offset |
|-------|-----------------|---------------|
| receiver | 0 | 0 |
| sampleRate | 4 | 4 |
| format | 8 | 8 |
| codec | 12 | 12 |
| crc | 16 | 16 |
| length | 20 | 20 |
| type | 24 | 24 |
| **channels** | **28** | **28 (= reserv[0])** |
| reserved[0-7] | 32-63 | reserv[1-8] = 32-63 |

**WSJT-X puts `channels` in `reserv[0]`** — it doesn't explicitly read a `channels` field. It reads `pStream->length` for sample count and uses internal state for channel count. So the layout is binary-compatible, but WSJT-X **ignores the channels field** in received frames. No mismatch.

However, WSJT-X **sends TX frames without setting channels** (it's in `reserv[0]` which is zero-initialized). AetherSDR reads `hdr.channels` from byte offset 28. If WSJT-X doesn't set it, `hdr.channels` will be 0 in received TX frames. Need to verify: **does `onBinaryMessage` handle channels=0?**

Looking at the code (TciServer.cpp line 331): `if (hdr.channels == 2)` and `else if (hdr.channels == 1)` — **channels=0 falls through both checks**, and no audio is forwarded. This could be a TX bug.

**Verify**: Log `hdr.channels` in the TX binary frame logger (Phase 1E).

---

## Phase 3: Root Cause Confirmation & Fixes

Apply fixes only after diagnostic data confirms the hypothesis. Prioritized by likelihood.

### Fix 1 (RX): Apply per-client resampling in `onDaxAudioReady()`

**Confirmed by**: Phase 2 Test A showing 24kHz frames sent when WSJT-X expects 48kHz, AND no `audio_samplerate` negotiation from WSJT-X.

**File**: `src/core/TciServer.cpp`, function `onDaxAudioReady()` (lines 476-508)

**Change**: Replace the single-frame broadcast with a per-client loop matching `onRxAudioReady()` pattern: apply `cs.resampler`, respect `cs.audioSampleRate`, `cs.audioFormat`, `cs.audioChannels`. Use `trx` (from DAX channel) instead of hard-coded 0 for receiver field.

### Fix 2 (RX): Negotiate 48kHz for WSJT-X compatibility

**Confirmed by**: WSJT-X not sending `audio_samplerate:48000;` but expecting it.

**Option A**: In init burst, advertise `audio_samplerate:48000;` instead of 24000. This means the default ClientState becomes 48kHz, and the resampler is created by default. Any client that prefers 24kHz can negotiate down.

**Option B**: Keep 24kHz default, but ensure the binary frame header `sampleRate` field truthfully reflects the actual rate (already does). WSJT-X would need to handle 24kHz correctly — but we can't control WSJT-X's behavior.

**Recommendation**: Option A is safer since WSJT-X expects 48kHz and the TCI spec lists 48kHz as a valid rate.

### Fix 3 (TX): Handle `channels=0` in `onBinaryMessage()`

**Confirmed by**: Phase 1E log showing `hdr.channels=0` from WSJT-X TX frames.

**File**: `src/core/TciServer.cpp`, function `onBinaryMessage()` (lines 310-378)

**Change**: Treat `channels=0` as stereo (default per TCI spec). Add before the format checks:
```cpp
int channels = (hdr.channels == 0) ? 2 : hdr.channels;
```

### Fix 4 (TX): Gate `feedDaxTxAudio()` for TCI timing

**Confirmed by**: Phase 1D log showing `m_radioTransmitting=false` when TCI TX audio first arrives.

**File**: `src/core/AudioEngine.cpp`, line 1205

**Change**: Allow audio to flow when either the radio is transmitting OR we locally requested TX:
```cpp
if (!m_radioTransmitting && !m_transmitting) {
```

### Fix 5 (TX): Handle missing `,tci` argument

**Confirmed by**: Log showing WSJT-X sends `trx:0,true;` without `,tci` because AetherSDR isn't identified as ExpertSDR3.

AetherSDR doesn't use the ExpertSDR3 TCI audio source model (it uses VITA-49 DAX TX). The `tci` argument is ExpertSDR3-specific and tells that server to route TCI audio to TX. AetherSDR's architecture routes TCI TX audio through `feedDaxTxAudio()` regardless, so this argument isn't needed — **but we should verify the audio actually reaches the VITA-49 TX path.**

### Fix 6 (TX): Ensure DAX TX stream exists

**Confirmed by**: Phase 1D log showing `m_txStreamId=0`.

If `m_txStreamId` is 0, no VITA-49 TX packets can be sent. The TX stream is created by `stream create type=dax_tx` which is normally triggered by the platform DAX bridge (`startDax()`). When only TCI is used (no platform DAX), this stream may never be created.

**File**: `src/core/TciServer.cpp` or `src/gui/MainWindow.cpp`

**Change**: When TCI receives the first TX binary frame, ensure a DAX TX stream exists. If `m_txStreamId == 0`, request one from the radio.

---

## Phase 4: Verification After Fixes

Re-run Phase 2 tests (A, B, C) and verify:
- [ ] RX: FT8 signals decode in WSJT-X (not just waterfall traces)
- [ ] TX: TUNE produces RF output (power meter > 0W)
- [ ] TX: FT8 CQ transmits actual signal (verifiable on second receiver/WebSDR)
- [ ] No regression: platform DAX (PipeWire/VirtualAudioBridge) still works
- [ ] No regression: voice TX (physical mic) still works
- [ ] Regression test for #752: external PTT + DAX TX still routes audio

---

## Files to Modify

| File | Change | Phase |
|------|--------|-------|
| `src/core/TciServer.cpp` | Add diagnostic logging (1A-1E) | Phase 1 |
| `src/core/AudioEngine.cpp` | Add TX flow logging (1D) | Phase 1 |
| `src/core/TciServer.cpp` | Per-client resampling in `onDaxAudioReady` | Phase 3 |
| `src/core/TciServer.cpp` | Handle `channels=0` in `onBinaryMessage` | Phase 3 |
| `src/core/AudioEngine.cpp` | Fix TX gate in `feedDaxTxAudio` | Phase 3 |
| `src/core/TciProtocol.cpp` | Change init burst default to 48kHz | Phase 3 |

## Reference Files (read-only)
- `src/core/TciServer.h` — ClientState struct (line 68-78)
- `src/core/TciProtocol.cpp` — init burst (line 63-151), cmdTrx (line 331-356)
- `src/core/Resampler.h` — `processStereoToStereo()` API
- `src/gui/MainWindow.cpp` — signal wiring (lines 1970-1976)
- TCI Spec v2.0 PDF: `/tmp/tci-spec.txt`
- WSJT-X Improved: `/tmp/wsjtx-improved/Transceiver/TCITransceiver.cpp`
