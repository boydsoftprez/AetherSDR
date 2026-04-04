#include "GpuSpectrumRenderer.h"

#include <rhi/qrhi.h>
#include <QFile>
#include <QDebug>

namespace AetherSDR {

// Fullscreen quad: 2 triangles, each vertex is (x, y, u, v)
static constexpr float kQuadVertices[] = {
    // pos        // uv
    -1.0f,  1.0f, 0.0f, 0.0f,   // top-left
    -1.0f, -1.0f, 0.0f, 1.0f,   // bottom-left
     1.0f,  1.0f, 1.0f, 0.0f,   // top-right
     1.0f, -1.0f, 1.0f, 1.0f,   // bottom-right
};

static QShader loadShader(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "GpuSpectrumRenderer: failed to load shader" << path;
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

GpuSpectrumRenderer::GpuSpectrumRenderer(QWidget* parent)
    : QRhiWidget(parent)
{
#if defined(Q_OS_MACOS)
    setApi(Api::Metal);
#elif defined(Q_OS_WIN)
    setApi(Api::Direct3D11);
#else
    setApi(Api::OpenGL);
#endif

    // QRhiWidget needs a native window handle to create its GPU surface.
    // This creates a native child window that sits on top of QPainter content.
    // Overlays (passband, slice lines, etc.) must be drawn by a transparent
    // overlay widget stacked above this one — see SpectrumWidget integration.
    setAttribute(Qt::WA_NativeWindow);
}

void GpuSpectrumRenderer::uploadRow(const quint32* bgraRow, int row, int width)
{
    if (width <= 0 || !bgraRow) return;

    // Using BGRA8 texture format — qRgb() values (0xAARRGGBB, BGRA in memory
    // on little-endian) can be uploaded directly with no byte swapping.
    PendingRow pr;
    pr.row = row;
    pr.data = QByteArray(reinterpret_cast<const char*>(bgraRow), width * 4);
    m_pendingRows.append(std::move(pr));

    // Request a repaint so render() picks up pending rows
    update();
}

void GpuSpectrumRenderer::setWaterfallSize(int width, int height)
{
    if (width == m_requestedWidth && height == m_requestedHeight)
        return;
    m_requestedWidth = width;
    m_requestedHeight = height;
    m_needsTextureRecreate = true;
    update();
}

void GpuSpectrumRenderer::createWaterfallTexture()
{
    QRhi* r = rhi();
    if (!r) return;

    // Clean up old texture
    if (m_wfTexture) {
        m_wfTexture->destroy();
        delete m_wfTexture;
        m_wfTexture = nullptr;
    }

    m_texWidth = m_requestedWidth;
    m_texHeight = m_requestedHeight;

    if (m_texWidth <= 0 || m_texHeight <= 0) return;

    m_wfTexture = r->newTexture(QRhiTexture::BGRA8,
                                QSize(m_texWidth, m_texHeight));
    if (!m_wfTexture->create()) {
        qWarning() << "GpuSpectrumRenderer: failed to create waterfall texture"
                   << m_texWidth << "x" << m_texHeight;
        delete m_wfTexture;
        m_wfTexture = nullptr;
        return;
    }

    // Clear texture to black — queue as a pending full-texture upload.
    // The actual GPU upload happens in the next render() pass.
    PendingRow fullClear;
    fullClear.row = -1;  // sentinel: means "full texture upload"
    fullClear.data = QByteArray(m_texWidth * m_texHeight * 4, '\0');
    m_pendingRows.clear();
    m_pendingRows.append(std::move(fullClear));

    // Rebuild SRB with new texture
    if (m_srb) {
        m_srb->destroy();
        delete m_srb;
    }
    m_srb = r->newShaderResourceBindings();
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage
                                                    | QRhiShaderResourceBinding::FragmentStage,
                                                 m_ubuf),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                   m_wfTexture, m_sampler),
    });
    m_srb->create();

    // Rebuild pipeline with new SRB
    if (m_pipeline) {
        m_pipeline->destroy();
        delete m_pipeline;
    }

    m_pipeline = r->newGraphicsPipeline();
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, loadShader(":/shaders/waterfall.vert.qsb") },
        { QRhiShaderStage::Fragment, loadShader(":/shaders/waterfall.frag.qsb") },
    });

    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({
        { 4 * sizeof(float) }  // stride: x, y, u, v
    });
    inputLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, 0 },                    // position
        { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },    // texcoord
    });
    m_pipeline->setVertexInputLayout(inputLayout);
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderResourceBindings(m_srb);
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    m_pipeline->create();

    m_needsTextureRecreate = false;
    m_textureCleared = false;  // wait for clear upload before drawing quad  // old rows don't match new texture dimensions

    qDebug() << "GpuSpectrumRenderer: waterfall texture created"
             << m_texWidth << "x" << m_texHeight;
}

void GpuSpectrumRenderer::initialize(QRhiCommandBuffer* cb)
{
    Q_UNUSED(cb);

    QRhi* r = rhi();
    if (!r) {
        qWarning() << "GpuSpectrumRenderer: QRhi not available — GPU rendering disabled";
        m_initialized = false;
        return;
    }

    qDebug() << "GpuSpectrumRenderer: initializing on"
             << r->backendName()
             << "driver:" << r->driverInfo().deviceName;

    // Vertex buffer (fullscreen quad)
    m_vbuf = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                          sizeof(kQuadVertices));
    m_vbuf->create();

    // Uniform buffer
    m_ubuf = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                          4 * sizeof(float));  // writeRow, texHeight, pad, pad
    m_ubuf->create();

    // Sampler (linear filtering, clamp-to-edge)
    m_sampler = r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                              QRhiSampler::None,
                              QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    m_sampler->create();

    // Create initial waterfall texture if dimensions are known
    if (m_requestedWidth > 0 && m_requestedHeight > 0) {
        createWaterfallTexture();
    }

    m_initialized = true;
}

void GpuSpectrumRenderer::render(QRhiCommandBuffer* cb)
{
    if (!m_initialized)
        return;

    QRhi* r = rhi();

    // Handle texture resize
    if (m_needsTextureRecreate && m_requestedWidth > 0 && m_requestedHeight > 0)
        createWaterfallTexture();

    // If no texture yet, just clear to black
    if (!m_wfTexture || !m_pipeline) {
        cb->beginPass(renderTarget(), Qt::black, {1.0f, 0});
        cb->endPass();
        return;
    }

    QRhiResourceUpdateBatch* u = r->nextResourceUpdateBatch();

    // Upload vertex data (could be done once, but it's tiny)
    u->uploadStaticBuffer(m_vbuf, kQuadVertices);

    // Upload pending waterfall rows
    for (const auto& pr : m_pendingRows) {
        if (pr.row == -1 && pr.data.size() == m_texWidth * m_texHeight * 4) {
            // Full texture clear (from createWaterfallTexture)
            QRhiTextureSubresourceUploadDescription sub(pr.data.constData(),
                                                         pr.data.size());
            u->uploadTexture(m_wfTexture, QRhiTextureUploadEntry(0, 0, sub));
            m_textureCleared = true;
        } else if (pr.row >= 0 && pr.row < m_texHeight && pr.data.size() == m_texWidth * 4) {
            QRhiTextureSubresourceUploadDescription sub(pr.data.constData(),
                                                         pr.data.size());
            sub.setDestinationTopLeft(QPoint(0, pr.row));
            sub.setSourceSize(QSize(m_texWidth, 1));
            u->uploadTexture(m_wfTexture, QRhiTextureUploadEntry(0, 0, sub));
        }
    }
    m_pendingRows.clear();

    // Update uniforms
    float uniforms[4] = {
        static_cast<float>(m_writeRow) / static_cast<float>(m_texHeight),  // normalized writeRow
        static_cast<float>(m_texHeight),
        0.0f, 0.0f  // padding
    };
    u->updateDynamicBuffer(m_ubuf, 0, sizeof(uniforms), uniforms);

    // Render the waterfall quad (only after texture has been cleared to black)
    const QSize outputSize = renderTarget()->pixelSize();
    if (!m_textureCleared) {
        // Just clear to black and submit the pending uploads, don't draw the quad yet
        cb->beginPass(renderTarget(), Qt::black, {1.0f, 0}, u);
        cb->endPass();
        return;
    }
    cb->beginPass(renderTarget(), Qt::black, {1.0f, 0}, u);

    cb->setGraphicsPipeline(m_pipeline);
    cb->setViewport({ 0, 0,
                      static_cast<float>(outputSize.width()),
                      static_cast<float>(outputSize.height()) });
    cb->setShaderResources(m_srb);

    const QRhiCommandBuffer::VertexInput vbufBinding(m_vbuf, 0);
    cb->setVertexInput(0, 1, &vbufBinding);
    cb->draw(4);  // triangle strip, 4 vertices

    cb->endPass();
}

void GpuSpectrumRenderer::releaseResources()
{
    delete m_pipeline; m_pipeline = nullptr;
    delete m_srb; m_srb = nullptr;
    delete m_sampler; m_sampler = nullptr;
    delete m_wfTexture; m_wfTexture = nullptr;
    delete m_ubuf; m_ubuf = nullptr;
    delete m_vbuf; m_vbuf = nullptr;
    m_initialized = false;
    m_pendingRows.clear();
}

} // namespace AetherSDR
