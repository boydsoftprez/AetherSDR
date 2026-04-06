# Debug & Fix: TCI Audio with WSJT-X (Discussion #762)

## Context

**Discussion**: [ten9876/AetherSDR#762](https://github.com/ten9876/AetherSDR/discussions/762)
Multiple users report TCI audio broken on all platforms (Win11/macOS/Linux) in v0.8.4:
- **RX**: Signals on WSJT-X waterfall but FT8 does **not decode**
- **TX**: PTT keys radio but **no audio/RF** output
- Confirmed by K5WH (Win), LB2EG (all platforms), K6JS (Win11)
- LB2EG also confirms SDC skimmer gets no RX audio via TCI

**Related open issues**: #841 (macOS TCI audio distorted + rigctld broken)

---

## Empirical Evidence

### Source 1: TCI Protocol Spec v2.0 (ExpertSDR3/TCI, PDF)
- Binary header: 16 x uint32 = 64 bytes, C struct layout (native endian)
- Valid audio sample rates: 8/12/24/48 kHz
- Default format: float32, default channels: 2
- `length` field = number of real samples in data[] array
- TX audio source: `TRX:n,true,tci;` tells server to use TCI audio. Without `,tci`, mic is used
- TX_CHRONO (type=3): server sends timing frames, client responds with TX_AUDIO

### Source 2: WSJT-X Improved (`salyh/wsjtx-improved`, `Transceiver/TCITransceiver.cpp`)

**RX audio handling** (lines 864-976):
- `writeAudioData()` does NOT read `pStream->sampleRate` from the binary header
- Hardcodes `m_downSampleFactor = 4` (line 195, 423)
- Always applies 4x downsample via `fil4_()` to feed 12kHz FT8 decoder
- Decoder buffer sized for `NTMAX * 12000` (line 953)
- **Measured behavior**: 48kHz input → 4x downsample → 12kHz (correct). 24kHz input → 4x downsample → 6kHz (broken)

**TX audio framing** (`txAudioData()`, lines 890-904):
- `QByteArray tx; tx.resize(AudioHeaderSize + len*sizeof(float)*2)` — Qt zero-fills new bytes
- Sets: `receiver=0, sampleRate=48000, format=3, length=len, type=TxAudioStream(2)`
- Does NOT set offset 28 (`reserv[0]` in WSJT-X struct)
- **Measured value at offset 28**: 0 (from QByteArray zero-fill)

**TX audio via TxChrono** (lines 866-887):
- Only triggers when server sends TxChrono frames
- AetherSDR never sends TxChrono → this path is dead
- WSJT-X relies on `txAudioData()` for direct TX

**PTT command** (`do_ptt()`, lines 1074-1107):
- Checks if device == `"ExpertSDR3"` (line 818)
- AetherSDR advertises `device:FLEX-xxxx ...;` → WSJT-X sends `trx:0,true;` (no `,tci`)

**Sample rate negotiation**:
- `audioSampleRate = 48000u` (line 216) — hardcoded
- No code path sends `audio_samplerate:48000;` to the server
- Sends `audio_start:0;` to enable RX streaming

**Header struct layout** (`TCITransceiver.hpp`, lines 24-35):
```
offset 0:  receiver    (uint32)
offset 4:  sampleRate  (uint32)
offset 8:  format      (uint32)
offset 12: codec       (uint32)
offset 16: crc         (uint32)
offset 20: length      (uint32)
offset 24: type        (uint32)
offset 28: reserv[9]   (9 x uint32 = 36 bytes, all zero)
offset 64: data[]      (float samples)
```

### Source 3: AetherSDR TCI Server (`src/core/TciServer.cpp`, `TciProtocol.cpp`)

**Init burst** (TciProtocol.cpp:63-151):
- Sends `device:FLEX-xxxx ...;` (not "ExpertSDR3")
- Sends `audio_samplerate:24000;`
- Never sends TX_CHRONO frames

**Client defaults** (TciServer.h:68-78):
- `audioSampleRate = 24000`, `audioChannels = 2`, `audioFormat = 3`
- `resampler = nullptr` (no resampling at 24kHz)

**Audio negotiation** (TciServer.cpp:216-232):
- Handles `audio_samplerate:` → creates Resampler if rate != 24000
- Since WSJT-X never sends this, client stays at 24kHz, no resampler created

**RX path A — `onRxAudioReady()`** (TciServer.cpp:382-472):
- Input: int16 stereo, 24kHz from PanadapterStream `audioDataReady` signal
- Per-client loop: applies `cs.resampler`, respects `cs.audioSampleRate/Format/Channels`
- If resampler is null (default), sends 24kHz

**RX path B — `onDaxAudioReady()`** (TciServer.cpp:476-508):
- Input: int16 stereo, 24kHz from PanadapterStream `daxAudioReady` signal
- Hard-codes `sampleRate=24000` in header
- Builds ONE frame, broadcasts to ALL clients — no per-client handling
- No resampler applied regardless of client settings

**Both paths wired** (MainWindow.cpp:1972-1975):
```cpp
connect(m_radioModel.panStream(), &PanadapterStream::audioDataReady,
        m_tciServer, &TciServer::onRxAudioReady);
connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady,
        m_tciServer, &TciServer::onDaxAudioReady);
```

**TX binary handler — `onBinaryMessage()`** (TciServer.cpp:310-378):
- Parses 64-byte header via memcpy
- Reads `hdr.channels` from offset 28

**Header struct layout** (TciServer.cpp:25-36):
```
offset 0:  receiver    (uint32)
offset 4:  sampleRate  (uint32)
offset 8:  format      (uint32)
offset 12: codec       (uint32)
offset 16: crc         (uint32)
offset 20: length      (uint32)
offset 24: type        (uint32)
offset 28: channels    (uint32)  ← WSJT-X sends 0 here
offset 32: reserved[8] (8 x uint32 = 32 bytes)
```

**TX routing by channels** (TciServer.cpp:328-377):
```cpp
if (hdr.format == 3) {
    if (hdr.channels == 2) { /* forward stereo */ }
    else if (hdr.channels == 1) { /* mono→stereo, forward */ }
    // channels == 0 → FALLS THROUGH, no audio forwarded
}
```

**AudioEngine TX gate** (AudioEngine.cpp:1197-1221):
- Low-latency route (default): `if (!m_radioTransmitting) { clear buffers; return; }`
- `m_radioTransmitting` set by interlock status from radio (async, delayed)
- `m_transmitting` set optimistically by `setTransmit()` (immediate)

---

## Confirmed Bugs (from code analysis, not inference)

### BUG A (TX): `channels=0` drops all WSJT-X TX audio

**Chain of evidence**:
1. WSJT-X `txAudioData()` creates QByteArray, resize zero-fills → offset 28 = 0
2. WSJT-X does not explicitly set any value at offset 28 (it's `reserv[0]`)
3. AetherSDR reads offset 28 as `hdr.channels`
4. `onBinaryMessage()` checks `hdr.channels == 2` then `hdr.channels == 1`
5. `channels == 0` matches neither → no code path forwards the audio
6. Result: **100% of TX audio from WSJT-X is silently discarded**

**Fix**: Treat `channels == 0` as stereo (TCI spec default is 2 channels).

### BUG B (RX): 24kHz audio sent to WSJT-X expecting 48kHz

**Chain of evidence**:
1. WSJT-X never sends `audio_samplerate:48000;` (no code path does this)
2. AetherSDR default ClientState rate = 24000, resampler = null
3. `onDaxAudioReady()` sends 24kHz frames (hard-coded)
4. `onRxAudioReady()` also sends 24kHz when resampler is null (which it is)
5. WSJT-X `writeAudioData()` applies hardcoded 4x downsample
6. 24000 / 4 = 6000 Hz effective rate into decoder
7. FT8 decoder expects 12000 Hz → **all timing and frequency bins wrong, decode fails**

**Fix**: Two parts needed:
- Change default `audioSampleRate` to 48000 in init burst and ClientState
- Fix `onDaxAudioReady()` to apply per-client resampling (matching `onRxAudioReady()` pattern)

---

## Open Questions (require runtime data)

| # | Question | Why it matters | How to measure |
|---|----------|---------------|----------------|
| Q1 | Which RX path fires — `onRxAudioReady`, `onDaxAudioReady`, or both? | Determines which code path needs the fix. If only `onRxAudioReady` fires, Fix B only needs the default rate change | Add periodic counters to both functions |
| Q2 | Is `m_txStreamId` non-zero when TCI is the only audio path? | If 0, VITA-49 TX packets can't be sent even after Bug A is fixed | Log `m_txStreamId` in `feedDaxTxAudio()` |
| Q3 | What are `m_radioTransmitting` and `m_transmitting` when TX audio arrives? | Determines if the feedDaxTxAudio gate is also blocking audio after Bug A fix | Log both flags at feedDaxTxAudio entry |
| Q4 | Is `m_daxTxUseRadioRoute` true or false? | Determines which TX code path is active (low-latency vs radio-native) | Log in `feedDaxTxAudio()` |
| Q5 | Do users have DAX enabled in the DIGI applet when using TCI? | Affects whether DAX TX stream exists and which RX path fires | Ask in discussion or log DAX state |

---

## Implementation Plan

### Step 1: Add diagnostic instrumentation

All logging uses existing `qCInfo(lcCat)` / `qCInfo(lcAudio)` categories.

**1A. Text command logger** — `TciServer::onTextMessage()` (line 193, inside command loop):
```cpp
qCInfo(lcCat) << "TCI <--" << ws->peerAddress().toString() << trimmed;
```
Measures: every command from WSJT-X. Confirms Q1 (audio_samplerate negotiation absent).

**1B. RX path counters** — both `onRxAudioReady()` (line 382) and `onDaxAudioReady()` (line 476):
```cpp
static int count = 0;
if (++count % 500 == 1)
    qCInfo(lcCat) << "TCI: <functionName> fired" << count << "times,"
                  << pcm.size() << "bytes," << m_clients.size() << "clients";
```
Measures: **Q1** — which path fires and at what rate.

**1C. RX frame header dump** — in BOTH `onRxAudioReady()` and `onDaxAudioReady()`, log the first frame sent:
```cpp
static bool first = true;
if (first) {
    first = false;
    qCInfo(lcCat) << "TCI: first RX frame from <functionName>:"
                  << "sampleRate=" << <rate> << "format=" << <fmt>
                  << "channels=" << <ch> << "length=" << <len>
                  << "frameBytes=" << frame.size();
}
```
Measures: exact header values sent to WSJT-X.

**1D. TX binary frame logger** — `onBinaryMessage()` (after header parse, line ~320):
```cpp
static int txFrameCount = 0;
if (++txFrameCount <= 5 || txFrameCount % 500 == 0)
    qCInfo(lcCat) << "TCI: TX binary frame #" << txFrameCount
                  << "receiver=" << hdr.receiver << "rate=" << hdr.sampleRate
                  << "format=" << hdr.format << "channels=" << hdr.channels
                  << "type=" << hdr.type << "length=" << hdr.length
                  << "payloadBytes=" << payloadBytes;
```
Measures: confirms `channels=0` from WSJT-X empirically (Bug A). Also captures rate/format.

**1E. TX AudioEngine logger** — `feedDaxTxAudio()` (top of function, before any gate):
```cpp
static int txCallCount = 0;
if (++txCallCount <= 5 || txCallCount % 200 == 0)
    qCInfo(lcAudio) << "feedDaxTxAudio #" << txCallCount
                    << "radioTx=" << m_radioTransmitting
                    << "tx=" << m_transmitting
                    << "daxTxMode=" << m_daxTxMode
                    << "radioRoute=" << m_daxTxUseRadioRoute
                    << "txStreamId=" << m_txStreamId
                    << "bytes=" << pcm.size();
```
Measures: **Q2**, **Q3**, **Q4** — all TX gate state at the moment audio arrives.

**1F. DAX state logger** — when TCI audio starts (`onTextMessage`, after `audio_start` handling):
```cpp
qCInfo(lcCat) << "TCI: audio_start — DAX state:"
              << "txStreamId=" << (m_audio ? m_audio->txStreamId() : 0)
              << "daxTxMode=" << (m_audio ? m_audio->daxTxMode() : false);
```
Measures: **Q2/Q5** — whether DAX TX stream exists when TCI audio begins.

### Step 2: Build, run, collect data

1. Build with instrumentation: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j$(nproc)`
2. Kill any running AetherSDR, launch instrumented build
3. Connect to FlexRadio, DIGU on 14.074 MHz
4. Enable TCI in DIGI applet
5. Start WSJT-X Improved with TCI rig
6. **Collect RX data**: watch log for 1A/1B/1C output, observe WSJT-X waterfall + decode status
7. **Collect TX data**: click TUNE in WSJT-X, watch log for 1D/1E/1F output, observe power meter
8. Save log output to `docs/tci-debug-log-phase1.txt`

### Step 3: Apply fixes (only after Step 2 data confirms each bug)

**Fix A — TX channels=0** (Bug A, confirmed by code + Step 2 1D log):

File: `src/core/TciServer.cpp`, function `onBinaryMessage()`, line ~327

Before the format checks, normalize channels:
```cpp
int channels = (hdr.channels == 0) ? 2 : static_cast<int>(hdr.channels);
```
Then use `channels` instead of `hdr.channels` in all subsequent checks.

**Fix B — RX default sample rate** (Bug B, confirmed by code + Step 2 1A/1C log):

File: `src/core/TciProtocol.cpp`, line 147 — change init burst:
```cpp
burst += QStringLiteral("audio_samplerate:48000;");
```

File: `src/core/TciServer.h`, line 72 — change default:
```cpp
int audioSampleRate{48000};
```

File: `src/core/TciServer.cpp`, `onNewConnection()` area — create resampler by default:
```cpp
cs.resampler = new Resampler(24000.0, 48000, 4096);
```

**Fix C — `onDaxAudioReady()` per-client resampling** (confirmed by Step 2 Q1 showing this path fires):

File: `src/core/TciServer.cpp`, function `onDaxAudioReady()` (lines 476-508)

Replace single-frame broadcast with per-client loop matching `onRxAudioReady()`:
- Iterate `m_clients`, skip if `!cs.audioEnabled`
- Apply `cs.resampler` via `processStereoToStereo()`
- Respect `cs.audioSampleRate`, `cs.audioFormat`, `cs.audioChannels`
- Use `trx` (from DAX channel) for receiver field

**Fix D — TX gate race** (only if Step 2 Q3 shows `m_radioTransmitting=false, m_transmitting=true` when audio arrives):

File: `src/core/AudioEngine.cpp`, line 1205

Change from:
```cpp
if (!m_radioTransmitting) {
```
To:
```cpp
if (!m_radioTransmitting && !m_transmitting) {
```

**Fix E — DAX TX stream creation** (only if Step 2 Q2 shows `m_txStreamId=0`):

Ensure a DAX TX stream is created when TCI receives the first TX binary frame.
Implementation depends on Step 2 findings — may require `stream create type=dax_tx` command to radio.

### Step 4: Build, re-test, measure

Re-run Step 2 with fixes applied. Collect same measurements. Verify:
- [ ] 1C log shows `sampleRate=48000` in RX frames
- [ ] 1D log shows `channels=0` but audio is now forwarded (add log line confirming forward)
- [ ] 1E log shows `feedDaxTxAudio` is called AND not gated/dropped
- [ ] WSJT-X waterfall shows signals AND FT8 decodes appear
- [ ] TUNE produces RF output (power meter > 0W)

### Step 5: Regression verification

- [ ] Platform DAX (PipeWire on Linux, VirtualAudioBridge on macOS) + WSJT-X still works
- [ ] Voice TX (physical mic, non-DAX) still works
- [ ] #752 scenario: external PTT + DAX TX still routes audio
- [ ] TCI client requesting 24kHz explicitly still gets 24kHz (not broken by default change)
- [ ] Multiple TCI clients with different sample rates get correct audio

---

## Files to Modify

| File | Change | Step |
|------|--------|------|
| `src/core/TciServer.cpp` | Diagnostic logging (1A-1D, 1F) | Step 1 |
| `src/core/AudioEngine.cpp` | Diagnostic logging (1E) | Step 1 |
| `src/core/TciServer.cpp` | Handle channels=0 in onBinaryMessage (Fix A) | Step 3 |
| `src/core/TciProtocol.cpp` | Change init burst default to 48kHz (Fix B) | Step 3 |
| `src/core/TciServer.h` | Change ClientState default to 48kHz (Fix B) | Step 3 |
| `src/core/TciServer.cpp` | Create resampler by default (Fix B) | Step 3 |
| `src/core/TciServer.cpp` | Per-client resampling in onDaxAudioReady (Fix C) | Step 3 |
| `src/core/AudioEngine.cpp` | TX gate fix (Fix D) — conditional | Step 3 |

## Reference Files (read-only)
- `src/core/TciServer.h` — ClientState struct (lines 68-78)
- `src/core/Resampler.h` — `processStereoToStereo()` API
- `src/gui/MainWindow.cpp` — signal wiring (lines 1970-1976)
- TCI Spec v2.0: `/tmp/tci-spec.txt`
- WSJT-X Improved: `/tmp/wsjtx-improved/Transceiver/TCITransceiver.cpp`
