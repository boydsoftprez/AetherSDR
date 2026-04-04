#pragma once

#include <QRhiWidget>
#include <QVector>

QT_FORWARD_DECLARE_CLASS(QRhi)
QT_FORWARD_DECLARE_CLASS(QRhiBuffer)
QT_FORWARD_DECLARE_CLASS(QRhiTexture)
QT_FORWARD_DECLARE_CLASS(QRhiSampler)
QT_FORWARD_DECLARE_CLASS(QRhiShaderResourceBindings)
QT_FORWARD_DECLARE_CLASS(QRhiGraphicsPipeline)
QT_FORWARD_DECLARE_CLASS(QRhiCommandBuffer)
QT_FORWARD_DECLARE_CLASS(QRhiResourceUpdateBatch)

namespace AetherSDR {

/// GPU-accelerated waterfall renderer using Qt6 QRhi.
///
/// Replaces the CPU QImage blit in SpectrumWidget::drawWaterfall() with a
/// GPU texture ring buffer. The CPU still does color mapping (dbmToRgb /
/// intensityToRgb) — we upload pre-mapped RGBA rows to a GPU texture and
/// render a fullscreen quad with UV offset for ring-buffer scrolling.
class GpuSpectrumRenderer : public QRhiWidget
{
    Q_OBJECT

public:
    explicit GpuSpectrumRenderer(QWidget* parent = nullptr);

    /// True after initialize() succeeds. If false, caller should
    /// fall back to the QPainter software rendering path.
    bool isReady() const { return m_initialized; }

    /// Upload a single RGBA scanline at the given row index.
    /// Called from SpectrumWidget::pushWaterfallRow() / updateWaterfallRow().
    /// @param rgbaRow  Pre-mapped RGBA pixels (width must match texture width)
    /// @param row      Ring buffer row index [0, textureHeight)
    void uploadRow(const quint32* rgbaRow, int row, int width);

    /// Notify that the ring buffer write position has changed.
    void setWriteRow(int row) { m_writeRow = row; }

    /// Request texture resize (e.g. on widget resize).
    void setWaterfallSize(int width, int height);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    void createWaterfallTexture();

    bool m_initialized{false};

    // GPU resources
    QRhiBuffer* m_vbuf{nullptr};          // fullscreen quad vertices
    QRhiBuffer* m_ubuf{nullptr};          // uniform buffer (writeRow, etc.)
    QRhiTexture* m_wfTexture{nullptr};    // waterfall RGBA8 ring buffer
    QRhiSampler* m_sampler{nullptr};
    QRhiShaderResourceBindings* m_srb{nullptr};
    QRhiGraphicsPipeline* m_pipeline{nullptr};

    // Pending row uploads (accumulated between frames)
    struct PendingRow {
        int row;
        QByteArray data;  // RGBA8 pixel data for one row
    };
    QVector<PendingRow> m_pendingRows;

    // State
    int m_writeRow{0};
    int m_texWidth{0};
    int m_texHeight{0};
    int m_requestedWidth{0};
    int m_requestedHeight{0};
    bool m_needsTextureRecreate{false};
    bool m_textureCleared{false};  // true after first full clear is submitted
};

} // namespace AetherSDR
