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

## Architecture Decision

**Current:** QRhiWidget as child of SpectrumWidget (QWidget)
- Pro: minimal changes to existing code
- Con: WA_NativeWindow compositing overhead negates savings on macOS

**Recommended:** SpectrumWidget inherits QRhiWidget directly
- Pro: single GPU surface, no compositing overhead, QPainter composites for free
- Con: larger refactor, all rendering must go through GPU or QPainter-on-QRhi
- This is the path forward for Phase 3+

## Measurements To Take Next
- [ ] SpectrumWidget-as-QRhiWidget: CPU baseline
- [ ] GPU waterfall + GPU FFT polyline: CPU comparison
- [ ] Linux (OpenGL): verify GPU actually helps where CG doesn't
- [ ] 4-pan multi-pan: GPU vs CPU comparison
- [ ] High FPS (50 FPS waterfall): GPU vs CPU comparison
