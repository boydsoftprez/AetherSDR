# GPU Spectrum Rendering — Empirical Notes

## Issue: #391

## Environment
- macOS 15 (Darwin 25.4.0), Apple M1 Pro
- Qt 6.11.0, Metal backend
- AetherSDR v0.7.18.6
- FLEX-6600 radio, local connection, 20m band

## Profiling Methodology
- All measurements: RelWithDebInfo build (optimized, no ASan)
- `ps -o %cpu` sampled every 10s over 40s after radio connect
- `sample` tool for per-function CPU breakdown (5s capture)
- Single pan, single slice, waterfall streaming at default FPS

---

## Measurement 1: Baseline (CPU-only, QPainterPath)

| Metric | Value |
|--------|-------|
| Total CPU | ~110% |
| RSS Memory | 298 MB |
| drawWaterfall | p.drawImage() ring buffer split blit |
| drawSpectrum | QPainterPath + fillPath + drawPath |

CPU sample (5s, 2702 total samples):
- paintEvent: dominant (not measured separately in baseline)
- CLAUDE.md states ~97% of main thread from waterfall on Linux

---

## Measurement 2: GPU Waterfall (QRhiWidget child, Metal)

### Phase 2a — Initial (QImage still written, RGBA byte swap)
| Metric | Value |
|--------|-------|
| Total CPU | ~121% (+11% vs baseline) |
| RSS Memory | 530 MB (+232 MB) |
| drawWaterfall | GPU textured quad (Metal) |
| drawSpectrum | QPainterPath (unchanged) |

**Finding:** GPU version uses MORE CPU. Causes:
1. CPU still writes to QImage (for fallback)
2. BGRA→RGBA byte swap per pixel per row
3. WaterfallOverlayWidget double-paints overlays
4. Native child window (`WA_NativeWindow`) compositing overhead

### Phase 2b — Optimized (skip QImage, BGRA8, no byte swap)
| Metric | Value |
|--------|-------|
| Total CPU | ~121% (same) |
| RSS Memory | 530 MB |

**Finding:** Skipping QImage write + eliminating byte swap had no measurable effect. The bottleneck is elsewhere.

CPU sample (5s, 2702 total samples):
- drawSpectrum: 960 samples (35%) — NOW the bottleneck
- drawWaterfall: 0 samples (0%) — ELIMINATED by GPU
- paintSiblingsRecursive: 1340 samples (50%) — Qt widget tree compositing
- Widget management overhead: ~380 samples (14%)

**Key insight:** The waterfall blit was successfully eliminated from CPU profile. But `paintSiblingsRecursive` (Qt's native child window compositing) costs ~50% of main thread. This overhead comes from having WA_NativeWindow child widgets (GpuSpectrumRenderer + WaterfallOverlayWidget) which force Qt to recursively traverse and composite the widget tree.

---

## Measurement 3: drawPolyline optimization

Replaced `QPainterPath` + `fillPath()` + `drawPath()` with `QPolygon` + `drawPolygon()` + `drawPolyline()`.

### CPU-only + drawPolyline
| Metric | Value |
|--------|-------|
| Total CPU | ~111% (same as baseline) |
| RSS Memory | 299 MB |

**Finding:** No measurable improvement in CPU-only mode. macOS Core Graphics hardware-accelerates QPainterPath internally — the path tessellation is already fast.

### GPU + drawPolyline
| Metric | Value |
|--------|-------|
| Total CPU | ~138% (+17% vs GPU baseline??) |
| RSS Memory | 524 MB |

CPU sample: drawSpectrum dropped from 960 → 685 samples (29% reduction in FFT rendering).

**Anomaly:** Total CPU increased despite drawSpectrum getting faster. Possible causes: measurement variance, thermal throttling changes, or the simpler drawPolyline triggers more frequent repaints.

---

## Key Findings

### macOS Core Graphics is already hardware-accelerated
- `p.drawImage()` on macOS uses Core Graphics which leverages the GPU internally
- The QImage ring buffer blit was NOT a pure CPU operation — it was already partially GPU-accelerated
- This is why eliminating it shows no CPU improvement: we replaced one GPU operation (CG blit) with another (Metal texture)

### Native child window overhead is significant
- `WA_NativeWindow` on child widgets forces Qt to use `paintSiblingsRecursive` for compositing
- This costs ~1400 samples/5s (~50% of main thread rendering time)
- Without native child windows, Qt paints directly to the backing store (much faster)

### QRhiWidget-as-base-class: 17% CPU but QPainter broken on Metal
- Making SpectrumWidget inherit QRhiWidget gave 17% CPU (vs 110% baseline)
- BUT `QPainter(this)` on QRhiWidget returns inactive on Qt 6.11 Metal
- Error: `QWidget::paintEngine: Should no longer be called`
- QPainter compositing on QRhiWidget is documented for Qt 6.7+ but does
  NOT work on Metal backend in Qt 6.11 — likely a Qt bug or limitation
- The GPU render() works, but paintEvent can't draw overlays → laggy display
- **This approach is parked** until Qt fixes QPainter-on-QRhiWidget with Metal

### Child QRhiWidget approach: working but overhead negates savings
- Child QRhiWidget + transparent overlay: visually correct, all overlays work
- But WA_NativeWindow compositing costs ~143% CPU (worse than 110% baseline)
- The compositing overhead from native child windows exceeds waterfall blit savings

### Remaining path forward
- All GPU rendering in render() (waterfall + FFT + overlays as GPU primitives)
- No QPainter at all on the QRhiWidget — text rendered to QImage, uploaded as texture
- OR: wait for Qt to fix QPainter-on-QRhiWidget with Metal backend
- OR: use QOpenGLWidget instead of QRhiWidget (older but QPainter compositing works)

### GPU waterfall IS working correctly
- Metal backend initializes on M1 Pro
- BGRA8 texture upload works (no byte swap needed)
- Ring buffer UV offset scrolling works
- Color mapping matches CPU path
- Transparent overlay widget for QPainter overlays works

### Reasons to continue with GPU beyond CPU savings
- **Higher refresh rates**: GPU can render 60+ FPS waterfall without CPU bottleneck
- **Resolution independence**: GPU texture scaling handles Retina/4K natively
- **Future features**: smooth zoom, animated frequency changes, GPU colormap customization
- **Linux/Windows**: those platforms DON'T have Core Graphics — the blit IS the bottleneck there
- **Multi-pan**: 4 pans at high FPS would overwhelm CPU but not GPU

---

---

## Measurement 4: SpectrumWidget as QRhiWidget + offscreen overlay

Made SpectrumWidget inherit QRhiWidget directly. All QPainter content (spectrum
line, grid, band plan, scales, slice markers, TNF, spots) rendered to an offscreen
QImage (`m_overlayCache`), uploaded as a second GPU texture, alpha-composited over
the waterfall in the GPU render pass.

### Key finding: paintEvent MUST call QRhiWidget::paintEvent()
Without calling the base class paintEvent, `initialize()` and `render()` are
NEVER called — QRhiWidget's GPU pipeline is triggered by its own paintEvent.

### Key finding: QPainter on QRhiWidget is broken on Metal
`QPainter(this)` on a QRhiWidget returns inactive painter on Qt 6.11 Metal.
Error: `QWidget::paintEngine: Should no longer be called`. The offscreen QImage
approach bypasses this entirely — QPainter draws to QImage (always works), then
the QImage is uploaded as a GPU texture.

### Results

| Config | CPU | Memory | Visual |
|--------|-----|--------|--------|
| QRhiWidget base, no overlay | ~17% | 298 MB | Waterfall only, no spectrum/overlays |
| QRhiWidget + overlay (2x Retina) | ~52% | 520 MB | All overlays, waterfall slow |
| QRhiWidget + overlay (1x) | ~37% | 509 MB | Smooth spectrum, blurry text, waterfall slow |
| QRhiWidget + overlay render in render() | ~52% | 520 MB | Waterfall even slower (render() blocked) |
| CPU-only baseline | ~110% | 298 MB | Everything smooth and fast |

### Issues resolved
1. **Waterfall scroll direction** — FIXED: flipped UV coords on waterfall quad
   (`v=1 at top, v=0 at bottom` in vertex data)
2. **Passband overlay invisible** — FIXED: premultiplied alpha blending
   (srcColor = One, not SrcAlpha)
3. **Waterfall quad positioning** — FIXED: NDC coordinates computed from
   spectrum/waterfall split, dynamic vertex buffer

### Issues remaining
1. **Waterfall scroll speed much slower than CPU version** — ~25 rows/sec
   data rate is SAME as CPU, but GPU version LOOKS slower. In CPU mode,
   p.drawImage() blits the entire QImage (including already-filled rows)
   instantly via Core Graphics hardware acceleration. GPU mode uploads
   rows one-at-a-time to a texture that starts black, so the progressive
   fill is visible. The fundamental issue: QRhiWidget render loop has
   overhead from Metal command buffer management that reduces effective FPS.
   
2. **Text blurry at 1x overlay** — rendering overlay at logical pixels (1x)
   makes text blurry on Retina. But 2x Retina means 5MB texture upload per
   frame which tanks performance. Need a middle ground.

3. **Overlay rendering cost** — QPainter renderOverlaysToImage() takes 6ms
   at 2x Retina, ~2ms at 1x. Called every FFT frame (~25/sec). Combined
   with 1.3MB (1x) or 5.3MB (2x) texture upload per frame, this dominates
   the render budget.

### Root cause analysis: why GPU is slower than CPU on macOS

The core issue is that **macOS Core Graphics already GPU-accelerates QPainter**.
When the CPU path calls `p.drawImage()`, `p.fillPath()`, `p.drawPolyline()`,
Core Graphics sends these to the GPU internally via Metal. The CPU is mostly
idle during rendering — Core Graphics does the compositing.

Our QRhi approach replaces one Metal path (Core Graphics → Metal) with another
(QRhi → Metal) but adds overhead:
- QPainter → QImage offscreen render (CPU-bound, no CG acceleration)
- QImage → GPU texture upload (memory copy, ~1-5 MB/frame)
- QRhiWidget render loop (Metal command buffer setup/teardown)

On Linux/Windows, Core Graphics doesn't exist. QPainter renders via software
rasterizer (CPU-bound). There, QRhi would provide a real speedup because the
GPU blit replaces a CPU blit. **This optimization is primarily for Linux.**

### Possible paths forward

**A) Ship as-is with ENABLE_RHI=OFF default on macOS**
- macOS: CPU path with Core Graphics (fast, 110% CPU but that's CG doing GPU work)
- Linux: GPU path with QRhi (would be faster than CPU on Linux)
- Simplest, no visual regressions

**B) Split overlay into static + dynamic layers**
- Static: grid, band plan, scales, background → render once, upload once
- Dynamic: spectrum line only → render to small QImage (~300px height), upload each frame
- Reduces per-frame overlay cost from 5MB to ~0.5MB

**C) Move ALL rendering to GPU shaders (no QPainter)**
- Spectrum line as vertex buffer + line draw
- Grid as line primitives
- Text pre-rendered to texture atlas
- Most complex but eliminates QPainter entirely

**D) Use QOpenGLWidget instead of QRhiWidget**
- QPainter compositing works on QOpenGLWidget (proven Qt feature)
- OpenGL deprecated on macOS but still functional
- Simpler integration, no offscreen QImage needed
4. **Overlay uploaded every FFT frame** — could be optimized with dirty flag to skip
   upload when overlays haven't changed

### Architecture (current working approach)

```
SpectrumWidget : QRhiWidget
  │
  ├─ initialize() — create GPU resources (vbuf, ubuf, textures, pipelines)
  ├─ render()     — upload waterfall rows + overlay QImage, draw two quads
  │   1. Waterfall quad (BGRA8 ring buffer texture, UV scroll)
  │   2. Overlay quad (BGRA8 from QImage, alpha blended)
  │
  ├─ paintEvent() — calls QRhiWidget::paintEvent() then returns
  │   (QPainter on QRhiWidget doesn't work on Metal)
  │
  ├─ renderOverlaysToImage() — QPainter → m_overlayCache QImage
  │   draws: background, grid, spectrum, band plan, scales,
  │   divider, time scale, TNF, spots, slice markers
  │   waterfall area left transparent for GPU waterfall to show through
  │
  └─ gpuUploadRow() — queue BGRA row for GPU texture upload
```

### Remaining work
- Fix waterfall scroll direction (shader UV offset sign)
- Fix waterfall rendering speed/smoothness
- Fix Retina DPR for overlay QImage (blurry text)
- Optimize overlay uploads (dirty flag, skip when unchanged)
- Test mouse events (click-to-tune, scroll) — QRhiWidget inherits QWidget events
- Test VFO widget positioning (child QWidgets on QRhiWidget)
- Remove debug render() frame counter
- Profile and compare with baseline

## Measurements To Take Next
- [x] SpectrumWidget-as-QRhiWidget: ~17-29% CPU (confirmed)
- [x] After waterfall fix: smooth scrolling confirmed at 47-50 FPS render rate
- [x] After Retina DPR fix: text sharp at 2x device pixel ratio
- [ ] Linux (OpenGL): verify GPU actually helps where CG doesn't
- [ ] 4-pan multi-pan: GPU vs CPU comparison
- [ ] High FPS (50 FPS waterfall): GPU vs CPU comparison

---

## Measurement 5: Final working state (two-quad waterfall + Retina overlay)

| Metric | Value |
|--------|-------|
| CPU % | ~39-52% (RelWithDebInfo) |
| render() FPS | 47-50 |
| Waterfall scroll | Smooth, correct direction, seeded from QImage |
| Spectrum line | Smooth via QPainter overlay |
| Text sharpness | Sharp (2x Retina overlay) |
| Passband overlay | Visible with correct alpha |
| Resize | No pink flash (repaint() fix) |
| Startup | Brief pink flash (known limitation) |

## Known Issue: Startup Pink Flash (QRhiWidget Metal)

**Symptom:** Brief magenta/pink flash (~1-2 frames) when the app first
launches. Does NOT occur on resize (fixed with repaint()).

**Root cause:** QRhiWidget on Metal creates a native window with a GPU
backing texture that contains uninitialized video memory. Metal does not
zero-initialize textures. The pink color is raw GPU memory displayed for
1-2 frames before our first render() call clears it to black.

**Attempted fixes (all failed):**
1. QPalette background color — ignored by QRhiWidget (renders via GPU, not backing store)
2. Splash cover widget with WA_NativeWindow — can't stack above QRhiWidget's native surface
3. Deferred WA_NativeWindow via QTimer — breaks GPU initialization entirely
4. setFixedColorBufferSize(1,1) then switch — breaks rendering, widget never appears
5. Clear in initialize() via beginPass — renderTarget() not ready during initialize

**Why resize works but startup doesn't:** On resize, we call repaint()
which forces synchronous render() before the compositor shows the frame.
On startup, the widget is first shown by the layout system BEFORE any
paint event fires — there's no opportunity to call render() before the
first compositor frame.

**Possible future fixes:**
- Qt bug report: QRhiWidget should zero-initialize its backing texture on Metal
- Use QQuickWidget with QQuickRenderControl instead of QRhiWidget
- Parent widget paints black behind SpectrumWidget before it's shown
- Override QRhiWidget::event() to intercept the Show event and force a render
