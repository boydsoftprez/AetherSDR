# TCI Protocol v2.0 Compliance Audit & Remediation Plan

## Context

Reviewed the official [TCI Protocol v2.0 spec](https://github.com/ExpertSDR3/TCI/blob/main/TCI%20Protocol.pdf) (41 pages, dated 12 Jan 2024) against AetherSDR's TCI implementation. The implementation has 72 command handlers and works with WSJT-X/JTDX for basic FT8 workflows, but has several argument format mismatches, missing commands, and unit/scale discrepancies compared to the spec.

**Primary TCI clients:** WSJT-X, JTDX (parse responses positionally — format mismatches can crash them).

---

## P0: Client-Breaking Format Mismatches

### P0-1: DRIVE / TUNE_DRIVE missing TRX arg
- **File:** `src/core/TciProtocol.cpp` — `cmdDrive()` (line 414), `cmdTuneDrive()` (line 435)
- **Spec:** `DRIVE:<trx>,<power>` / `TUNE_DRIVE:<trx>,<power>`
- **Current:** GET returns `drive:<power>` (no trx), SET parses `args[0]` as power
- **Init burst already uses correct format** (`drive:%1,%2`) — handlers are inconsistent
- **Fix:** GET: return `drive:<trx>,<power>;` SET: parse args[0] as trx, args[1] as power

### P0-2: Protocol version — bump 1.5 to 2.0
- **File:** `src/core/TciProtocol.cpp` line 66
- **Current:** `protocol:ExpertSDR3,1.5;`
- **Fix:** `protocol:ExpertSDR3,2.0;`
- Clients may gate v2.0 features on this string

### P0-3: TRX command — accept arg3 (signal source)
- **File:** `src/core/TciProtocol.cpp` — `cmdTrx()` (line 339)
- **Spec:** `TRX:<trx>,<true/false>[,<source>]` where source = `tci|mic1|mic2|micpc|ecoder2`
- **Current:** Ignores arg3, always uses mic
- **Fix:** Parse optional arg3. When source=`tci`, this confirms client will send TX audio via binary stream (already handled by TX_CHRONO, but echo the source in response for client verification)

---

## P1: Protocol Compliance — Interoperability

### P1-1: VOLUME — wrong units and extra arg
- **File:** `src/core/TciProtocol.cpp` — `cmdVolume()` (line 710)
- **Spec:** `VOLUME:<dB>` range -60 to 0, no trx arg. Global volume.
- **Current:** Returns `volume:<trx>,<0-100>` (FlexRadio audioGain scale)
- **Fix:** Remove trx arg. Convert 0-100 <-> dB with helpers:
  - `gainToDb(gain) = (gain/100)*60 - 60`
  - `dbToGain(db) = (db+60)*100/60`

### P1-2: MUTE — remove extra trx arg
- **File:** `src/core/TciProtocol.cpp` — `cmdMute()` (line 737)
- **Spec:** `MUTE:<true/false>` (global, no trx)
- **Current:** Returns `mute:<trx>,<bool>`
- **Fix:** Remove trx from response. Apply to global audio mute (all slices).

### P1-3: RX_VOLUME — add channel arg, convert to dB
- **File:** `src/core/TciProtocol.cpp` — `cmdRxVolume()` (line 1193)
- **Spec:** `RX_VOLUME:<trx>,<channel>,<dB>` (-60 to 0)
- **Current:** Returns `rx_volume:<trx>,<0-100>` (missing channel, wrong units)
- **Fix:** Insert channel=0. Use dB conversion.

### P1-4: RX_BALANCE — add channel arg
- **Spec:** `RX_BALANCE:<trx>,<channel>,<balance>`
- **Current:** Missing channel arg
- **Fix:** Insert channel=0 in responses

### P1-5: AGC_MODE — map FlexRadio values to TCI spec
- **File:** `src/core/TciProtocol.cpp` — `cmdAgcMode()` (line 765)
- **Spec values:** `off`, `normal`, `fast`
- **FlexRadio values:** `off`, `slow`, `med`, `fast`
- **Fix:** Map: `slow->normal`, `med->normal`, `fast->fast`, `off->off`. Reverse: `normal->med`.

### P1-6: SPOT_DELETE — accept 1-arg form
- **File:** `src/core/TciProtocol.cpp` — `cmdSpotDelete()` (line 979)
- **Spec:** `SPOT_DELETE:<callsign>` (1 arg, delete by callsign)
- **Current:** Requires 2 args (callsign + freq), rejects 1-arg calls
- **Fix:** When 1 arg, delete all spots matching callsign. Keep 2-arg as extension.

### P1-7: VFO_LOCK — support 3-arg form
- **Spec v2.0:** `VFO_LOCK:<trx>,<channel>,<status>` (3 args)
- **Current:** Aliased to `cmdLock()` (2-arg)
- **Fix:** Create `cmdVfoLock()` handler, accept 3 args, ignore channel (always 0 for FlexRadio)

### P1-8: RX_CHANNEL_ENABLE — add channel arg to response
- **Spec:** `RX_CHANNEL_ENABLE:<trx>,<channel>,<status>`
- **Current:** Returns `rx_channel_enable:<trx>,true` (missing channel)
- **Fix:** Return `rx_channel_enable:<trx>,0,true;`

### P1-9: SPLIT_ENABLE — implement SET
- **Spec:** Bidirectional (settable). When true, TX on different slice.
- **Current:** GET only
- **Fix:** When set true and 2+ slices exist, assign TX to other slice. No-op if only 1 slice.

### P1-10: AUDIO_START/STOP — accept receiver arg
- **File:** `src/core/TciServer.cpp` — `onTextMessage()`
- **Spec:** `AUDIO_START:<receiver>` / `AUDIO_STOP:<receiver>`
- **Current:** No arg parsed
- **Fix:** Parse receiver arg if present (ignore value for now, single-receiver)

---

## Audio Chain Compliance Issues

### A0: AUDIO_STREAM_SAMPLE_TYPE parsing — string vs numeric (CRITICAL BUG)
- **File:** `src/core/TciServer.cpp` lines 272-279
- **Spec:** `AUDIO_STREAM_SAMPLE_TYPE:float32;` / `AUDIO_STREAM_SAMPLE_TYPE:int16;` (string names)
- **Init burst already sends string:** `audio_stream_sample_type:float32;` (line 152 of TciProtocol.cpp)
- **Bug:** The negotiation handler does `trimmed.mid(colonIdx2 + 1).toInt()` — parses as integer. If a client sends `audio_stream_sample_type:float32;`, `toInt()` returns 0 (int16), **silently switching format to int16**. Only numeric `0` or `3` work.
- **Fix:** Parse the value as string. Map `"int16"`→0, `"int24"`→1, `"int32"`→2, `"float32"`→3. Also accept numeric values for backwards compat with any existing clients.
- **Response should echo string name**, not number: `audio_stream_sample_type:float32;`

### A1: DAX RX audio ignores client format negotiation
- **File:** `src/core/TciServer.cpp` — `onDaxAudioReady()` (line 543)
- **Bug:** Always sends float32 stereo regardless of client's negotiated `audioFormat` and `audioChannels`. `onRxAudioReady()` correctly respects these settings, but `onDaxAudioReady()` hardcodes float32 stereo.
- **Fix:** Apply the same format/channel conversion logic from `onRxAudioReady()`. Extract shared conversion into a helper to avoid duplication.

### A2: AUDIO_STREAM_SAMPLES not honored — packet size is fixed
- **File:** `src/core/TciServer.cpp` lines 301-304
- **Spec:** Client can set samples per packet (100-2048). Server should adjust. Default per rate: 48kHz=2048, 24kHz=1024, 12kHz=512, 8kHz=256.
- **Current:** Acknowledged but ignored — uses whatever packet size the AudioEngine emits.
- **Fix:** Store `audioStreamSamples` in `ClientState`. Buffer/split RX audio to match the requested packet size. This is a moderate refactor — may want to add a small ring buffer per client.

### A3: Missing int24 and int32 sample type support
- **File:** `src/core/TciServer.cpp` line 275
- **Spec:** Supports int16, int24, int32, float32
- **Current:** Only accepts `0` (int16) and `3` (float32). Rejects int24/int32.
- **Fix:** Add int24 (pack 3 bytes per sample) and int32 conversion paths. Low priority — no known TCI clients use these formats.

### A4: DIGL/DIGU complex signal handling
- **Spec (page 10):** "When DIGL/DIGU modes are selected, a complex signal will be transmitted if the number of channels is 2, but if the number of channels is 1 then it's going to be a real signal."
- **Current:** Audio is always real (L=R or mono downmix). In DIGL/DIGU stereo mode, spec expects I/Q (complex) audio, not L/R.
- **Fix:** When mode is DIGL/DIGU and channels=2, send the complex (I/Q) signal from the DSP pipeline instead of duplicate L/R. Requires access to the pre-demodulation signal. This is a significant change — may be N/A for FlexRadio since the radio handles demodulation.

### A5: TX audio resampler quality — double int16 conversion
- **File:** `src/core/TciServer.cpp` lines 409-440
- **Current:** TX path does float32→int16→resample→int16→float32. The intermediate int16 steps add quantization noise (~96dB SNR).
- **Impact:** Negligible for FT8/FT4 (8-FSK with coding gain). Could matter for SSB voice or VARA.
- **Fix:** Upgrade `Resampler` to accept float32 directly (r8brain works with double internally), eliminating the int16 round-trip. Or add a `processFloat()` method.

### A6: IQ_STREAM binary frames not implemented
- **Spec:** StreamType 0 = IQ_STREAM for binary I/Q data
- **Current:** IQ streaming is handled via DAX IQ channels (text command `iq_start`/`iq_stop`) which creates a VITA-49 IQ stream from the radio. TCI binary IQ frames (type=0) are not sent.
- **Impact:** Skimmers and IQ recording clients expect binary IQ frames. Currently they get nothing via the binary channel.
- **Fix:** When `iq_start` is active, route DAX IQ data into TCI binary frames with type=0. Moderate effort.

---

## P2: Missing Features (Stubs / Nice-to-Have)

| Command | Type | Impl Effort | Notes |
|---------|------|-------------|-------|
| CW_MACROS_SPEED_UP/DOWN | Client->Server | Low | Adjust current CW speed by arg WPM |
| RX_CLICKED_ON_SPOT | Server->Client | Low | v2.0 format adds trx/channel prefix |
| TX_FOOTSWITCH | Server->Client | Medium | Wire to interlock/PTT source |
| APP_FOCUS | Server->Client | Medium | Wire to QWindow focus events |
| CW_MACROS_EMPTY | Server->Client | Medium | CW queue drain notification |
| CALLSIGN_SEND | Server->Client | Medium | CW callsign transmitted notification |
| LINE_OUT_RECORDER_* | N/A | Stub | Already acknowledged as stubs. N/A for FlexRadio. |
| LINEOUT_STREAM (type=4) | Binary | Stub | N/A for FlexRadio |
| DDS set | N/A | Stub | Pan center is radio-controlled. Log only. |
| MON_VOLUME dB conversion | Low | Low | Same dB math as VOLUME |
| RX/TX_SENSORS_ENABLE interval arg | Low | Low | Parse optional interval, adjust timer per-client |
| IQ_SAMPLERATE 384kHz | Low | Low | Add 384000 to supported rates |

---

## Implementation Sequence

**Phase 1 — P0 + A0 critical fixes (test immediately with WSJT-X):**
1. Fix `audio_stream_sample_type` parsing (A0) — **this is a silent data corruption bug**
2. Fix `cmdDrive()` and `cmdTuneDrive()` TRX arg format (P0-1, P0-2)
3. Bump protocol version to 2.0 (P0-2)
4. Accept TRX source arg in `cmdTrx()` (P0-3)

**Phase 2 — Audio chain fixes:**
1. Fix DAX RX audio to respect client format negotiation (A1)
2. Extract shared RX audio format conversion helper (refactor for A1)
3. Store and honor `audioStreamSamples` packet size (A2) — add per-client buffering

**Phase 3 — P1 volume/mute/gain cluster:**
1. Add dB<->gain helpers
2. Fix `cmdVolume()`, `cmdMute()`, `cmdRxVolume()` units + args
3. Fix `cmdRxBalance()` channel arg

**Phase 4 — P1 remaining:**
1. AGC_MODE mapping
2. SPOT_DELETE 1-arg support
3. VFO_LOCK 3-arg form
4. RX_CHANNEL_ENABLE channel arg
5. SPLIT_ENABLE set support
6. AUDIO_START/STOP receiver arg

**Phase 5 — Audio quality + features (as time permits):**
1. TX resampler float32 path (A5)
2. IQ_STREAM binary frames (A6)
3. int24/int32 sample types (A3)
4. DIGL/DIGU complex signal (A4) — may be N/A for FlexRadio

**Phase 6 — P2 stubs as time permits**

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/core/TciProtocol.cpp` | All P0 and P1 command fixes |
| `src/core/TciProtocol.h` | Add `cmdVfoLock()`, dB helpers |
| `src/core/TciServer.cpp` | AUDIO_START/STOP receiver arg |

---

## Verification

1. **WSJT-X 2.7+**: Connect, verify init burst parses, full FT8 TX/RX cycle, no "TCI error" in log
2. **JTDX**: Same basic verification
3. **Manual WebSocket** (`websocat ws://localhost:50001`): Send GET commands, verify response format:
   - `drive:0;` -> `drive:0,<power>;`
   - `tune_drive:0;` -> `tune_drive:0,<power>;`
   - `volume;` -> `volume:<dB>;`
   - `mute;` -> `mute:<bool>;`
   - `rx_volume:0;` -> `rx_volume:0,0,<dB>;`
   - `agc_mode:0;` -> `agc_mode:0,<normal|fast|off>;`
   - `spot_delete:CALLSIGN;` -> accepted (1 arg)
