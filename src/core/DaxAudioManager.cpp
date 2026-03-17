#include "DaxAudioManager.h"

#ifdef HAVE_PIPEWIRE

#include "SpscRingBuffer.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <QDebug>
#include <QMetaObject>
#include <QProcess>
#include <QTimer>

#include <array>
#include <thread>
#include <atomic>
#include <cstring>

namespace AetherSDR {

static constexpr int SAMPLE_RATE = 24000;
static constexpr int CHANNELS    = 2;
static constexpr int RING_SIZE   = 131072;  // ~1.4s of float32 stereo @ 24 kHz

// ─── Per-channel state ───────────────────────────────────────────────────────

struct RxChannel {
    pw_stream*     stream{nullptr};
    spa_hook       listener{};
    SpscRingBuffer ring{RING_SIZE};
    std::atomic<bool> active{false};
    int            channel{0};  // 1-based
    uint32_t       moduleId{0}; // PulseAudio module ID for cleanup
    QString        sinkName;    // e.g. "aethersdr_dax_1"
};

struct TxChannel {
    pw_stream*     stream{nullptr};
    spa_hook       listener{};
    SpscRingBuffer ring{RING_SIZE};
    std::atomic<bool> active{false};
    DaxAudioManager* manager{nullptr};  // for signal emission
    uint32_t       moduleId{0};
};

// ─── PipeWire callbacks (RT context) ─────────────────────────────────────────

static void onRxProcess(void* userdata)
{
    auto* ch = static_cast<RxChannel*>(userdata);
    auto* buf = pw_stream_dequeue_buffer(ch->stream);
    if (!buf) return;

    auto* d = &buf->buffer->datas[0];
    auto* dst = static_cast<float*>(d->data);
    if (!dst) {
        pw_stream_queue_buffer(ch->stream, buf);
        return;
    }

    const int maxSamples = d->maxsize / (sizeof(float) * CHANNELS);
    const int bytesNeeded = maxSamples * sizeof(float) * CHANNELS;
    const int bytesRead = ch->ring.read(dst, bytesNeeded);

    // Fill remainder with silence if ringbuffer didn't have enough
    if (bytesRead < bytesNeeded)
        std::memset(reinterpret_cast<char*>(dst) + bytesRead, 0, bytesNeeded - bytesRead);

    d->chunk->offset = 0;
    d->chunk->stride = sizeof(float) * CHANNELS;
    d->chunk->size   = bytesNeeded;

    pw_stream_queue_buffer(ch->stream, buf);
}

static void onTxProcess(void* userdata)
{
    auto* tx = static_cast<TxChannel*>(userdata);
    auto* buf = pw_stream_dequeue_buffer(tx->stream);
    if (!buf) return;

    auto* d = &buf->buffer->datas[0];
    auto* src = static_cast<const float*>(d->data);
    if (!src || d->chunk->size == 0) {
        pw_stream_queue_buffer(tx->stream, buf);
        return;
    }

    tx->ring.write(src, d->chunk->size);
    pw_stream_queue_buffer(tx->stream, buf);

    // Notify Qt thread (safe from RT via eventfd on Linux)
    if (tx->manager)
        QMetaObject::invokeMethod(tx->manager, "daxTxDataReady", Qt::QueuedConnection);
}

static const pw_stream_events rxStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = onRxProcess,
};

static const pw_stream_events txStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = onTxProcess,
};

// ─── Impl ────────────────────────────────────────────────────────────────────

struct DaxAudioManager::Impl {
    pw_main_loop* loop{nullptr};
    pw_context*   context{nullptr};
    pw_core*      core{nullptr};

    std::array<RxChannel, 4> rx;
    TxChannel tx;

    std::thread thread;
    std::atomic<bool> running{false};

    // Build SPA audio format pod (float32 stereo 24 kHz)
    const spa_pod* buildAudioFormat(spa_pod_builder& b)
    {
        spa_audio_info_raw info{};
        info.format   = SPA_AUDIO_FORMAT_F32;
        info.channels = CHANNELS;
        info.rate     = SAMPLE_RATE;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        return spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
    }

    // Create a pw_stream on the PipeWire thread (required by PW API)
    struct CreateStreamArgs {
        Impl* impl;
        RxChannel* rxCh;      // non-null for RX
        TxChannel* txCh;      // non-null for TX
        QString targetName;
        QString nodeName;
        bool isInput;          // PW_DIRECTION_INPUT for TX capture
    };

    static int createStreamOnPwThread(struct spa_loop*, bool, uint32_t,
                                       const void*, size_t, void* data)
    {
        auto* args = static_cast<CreateStreamArgs*>(data);
        auto* impl = args->impl;

        pw_properties* props;
        if (args->targetName.isEmpty()) {
            props = pw_properties_new(
                PW_KEY_MEDIA_TYPE,     "Audio",
                PW_KEY_MEDIA_CATEGORY, args->isInput ? "Capture" : "Playback",
                PW_KEY_MEDIA_ROLE,     "Communication",
                PW_KEY_NODE_NAME,      args->nodeName.toUtf8().constData(),
                nullptr);
        } else {
            props = pw_properties_new(
                PW_KEY_MEDIA_TYPE,     "Audio",
                PW_KEY_MEDIA_CATEGORY, args->isInput ? "Capture" : "Playback",
                PW_KEY_MEDIA_ROLE,     "Communication",
                PW_KEY_NODE_NAME,      args->nodeName.toUtf8().constData(),
                PW_KEY_TARGET_OBJECT,  args->targetName.toUtf8().constData(),
                nullptr);
        }

        pw_stream* stream = pw_stream_new(impl->core,
            args->nodeName.toUtf8().constData(), props);
        if (!stream) {
            delete args;
            return 0;
        }

        if (args->rxCh) {
            args->rxCh->stream = stream;
            pw_stream_add_listener(stream, &args->rxCh->listener, &rxStreamEvents, args->rxCh);
        } else {
            args->txCh->stream = stream;
            pw_stream_add_listener(stream, &args->txCh->listener, &txStreamEvents, args->txCh);
        }

        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[1] = { impl->buildAudioFormat(b) };

        auto flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
        if (!args->targetName.isEmpty())
            flags |= PW_STREAM_FLAG_AUTOCONNECT;

        pw_stream_connect(stream,
            args->isInput ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
            PW_ID_ANY,
            static_cast<pw_stream_flags>(flags),
            params, 1);

        delete args;
        return 0;
    }

    static int destroyStreamOnPwThread(struct spa_loop*, bool, uint32_t,
                                        const void*, size_t, void* data)
    {
        auto* stream = static_cast<pw_stream*>(data);
        if (stream) pw_stream_destroy(stream);
        return 0;
    }
};

// ─── DaxAudioManager ─────────────────────────────────────────────────────────

DaxAudioManager::DaxAudioManager(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
    for (int i = 0; i < 4; ++i)
        m_impl->rx[i].channel = i + 1;
    m_impl->tx.manager = this;
}

DaxAudioManager::~DaxAudioManager()
{
    shutdown();
}

bool DaxAudioManager::init()
{
    pw_init(nullptr, nullptr);

    m_impl->loop = pw_main_loop_new(nullptr);
    if (!m_impl->loop) {
        qWarning() << "DaxAudioManager: failed to create PipeWire main loop";
        return false;
    }

    m_impl->context = pw_context_new(
        pw_main_loop_get_loop(m_impl->loop), nullptr, 0);
    if (!m_impl->context) {
        qWarning() << "DaxAudioManager: failed to create PipeWire context";
        pw_main_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
        return false;
    }

    m_impl->core = pw_context_connect(m_impl->context, nullptr, 0);
    if (!m_impl->core) {
        qWarning() << "DaxAudioManager: failed to connect to PipeWire";
        pw_context_destroy(m_impl->context);
        pw_main_loop_destroy(m_impl->loop);
        m_impl->context = nullptr;
        m_impl->loop = nullptr;
        return false;
    }

    // Run PipeWire main loop in a dedicated thread
    m_impl->running = true;
    m_impl->thread = std::thread([this]() {
        pw_main_loop_run(m_impl->loop);
    });

    qDebug() << "DaxAudioManager: PipeWire initialized";
    return true;
}

void DaxAudioManager::shutdown()
{
    if (!m_impl->loop) return;

    // Deactivate all channels
    for (int i = 1; i <= 4; ++i)
        deactivateRxChannel(i);
    deactivateTx();

    // Stop the main loop and join the thread
    m_impl->running = false;
    pw_main_loop_quit(m_impl->loop);
    if (m_impl->thread.joinable())
        m_impl->thread.join();

    if (m_impl->core) {
        pw_core_disconnect(m_impl->core);
        m_impl->core = nullptr;
    }
    if (m_impl->context) {
        pw_context_destroy(m_impl->context);
        m_impl->context = nullptr;
    }
    if (m_impl->loop) {
        pw_main_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
    }

    pw_deinit();
    qDebug() << "DaxAudioManager: PipeWire shut down";
}

bool DaxAudioManager::isAvailable() const
{
    return m_impl->loop != nullptr && m_impl->running;
}

void DaxAudioManager::feedDaxRx(int channel, const QByteArray& pcm)
{
    if (channel < 1 || channel > 4) return;
    auto& ch = m_impl->rx[channel - 1];
    if (!ch.active) return;

    // Convert int16 stereo LE → float32 stereo (PipeWire handles 24k↔48k resampling)
    const auto* src = reinterpret_cast<const int16_t*>(pcm.constData());
    const int totalSamples = pcm.size() / sizeof(int16_t);

    float buf[2048];
    float sumSq = 0.0f;
    int remaining = totalSamples;
    int srcIdx = 0;

    while (remaining > 0) {
        const int chunk = std::min(remaining, 2048);
        for (int i = 0; i < chunk; ++i) {
            buf[i] = src[srcIdx + i] / 32768.0f;
            sumSq += buf[i] * buf[i];
        }
        ch.ring.write(buf, chunk * sizeof(float));
        srcIdx += chunk;
        remaining -= chunk;
    }

    if (totalSamples > 0)
        emit daxRxLevel(channel, std::min(std::sqrt(sumSq / totalSamples), 1.0f));
}

QByteArray DaxAudioManager::readDaxTx(int maxBytes)
{
    if (!m_impl->tx.active) return {};

    // PipeWire delivers audio at 24 kHz (our stream's native rate, resampled from 48k sink)
    QByteArray out(maxBytes, Qt::Uninitialized);
    const int bytesRead = m_impl->tx.ring.read(out.data(), maxBytes);
    out.resize(bytesRead);

    if (bytesRead > 0) {
        const auto* samples = reinterpret_cast<const float*>(out.constData());
        const int n = bytesRead / sizeof(float);
        float sumSq = 0.0f;
        for (int i = 0; i < n; ++i)
            sumSq += samples[i] * samples[i];
        emit daxTxLevel(std::min(std::sqrt(sumSq / n), 1.0f));
    }

    return out;
}

void DaxAudioManager::activateRxChannel(int channel)
{
    if (channel < 1 || channel > 4) return;
    auto& ch = m_impl->rx[channel - 1];
    if (ch.active) return;
    if (!m_impl->loop) return;

    const auto sinkName = (channel == 1) ? QString("AetherSDR-Input")
                                         : QString("AetherSDR-Input-%1").arg(channel);
    const auto desc = (channel == 1) ? QString("AetherSDR Input")
                                     : QString("AetherSDR Input %1").arg(channel);

    // Create a PulseAudio null sink (works on both PulseAudio and PipeWire).
    // Apps will see the .monitor source as an audio input (e.g., in WSJT-X Input dropdown).
    QProcess pactl;
    pactl.start("pactl", {"load-module", "module-null-sink",
        QString("sink_name=%1").arg(sinkName),
        QString("sink_properties=device.description=\"%1\"").arg(desc),
        "rate=24000", "channels=2", "format=float32le"});
    pactl.waitForFinished(3000);
    if (pactl.exitCode() != 0) {
        qWarning() << "DaxAudioManager: pactl failed for channel" << channel
                   << pactl.readAllStandardError();
        return;
    }
    ch.moduleId = pactl.readAllStandardOutput().trimmed().toUInt();
    ch.sinkName = sinkName;

    // Create a PipeWire stream on the PW thread that feeds audio INTO the null sink.
    auto* args = new Impl::CreateStreamArgs{
        m_impl.get(), &ch, nullptr,
        sinkName,
        QString("%1_feed").arg(sinkName),
        false  // OUTPUT direction
    };
    pw_loop_invoke(pw_main_loop_get_loop(m_impl->loop),
                   Impl::createStreamOnPwThread, 0, nullptr, 0, false, args);

    ch.ring.reset();
    ch.active = true;
    qDebug() << "DaxAudioManager: activated RX channel" << channel
             << "sink:" << sinkName << "moduleId:" << ch.moduleId;
    emit channelStateChanged(channel, true);
}

void DaxAudioManager::deactivateRxChannel(int channel)
{
    if (channel < 1 || channel > 4) return;
    auto& ch = m_impl->rx[channel - 1];
    if (!ch.active) return;

    if (ch.stream && m_impl->loop) {
        pw_loop_invoke(pw_main_loop_get_loop(m_impl->loop),
                       Impl::destroyStreamOnPwThread, 0, nullptr, 0, false, ch.stream);
        ch.stream = nullptr;
    }
    if (ch.moduleId > 0) {
        QProcess::execute("pactl", {"unload-module", QString::number(ch.moduleId)});
        ch.moduleId = 0;
    }
    ch.active = false;
    ch.ring.reset();
    qDebug() << "DaxAudioManager: deactivated RX channel" << channel;
    emit channelStateChanged(channel, false);
}

bool DaxAudioManager::isRxChannelActive(int channel) const
{
    if (channel < 1 || channel > 4) return false;
    return m_impl->rx[channel - 1].active;
}

void DaxAudioManager::activateTx()
{
    if (m_impl->tx.active) return;
    if (!m_impl->loop) return;

    // Create a PulseAudio null sink for TX — apps write audio to it,
    // we read from its monitor source via PipeWire stream.
    QProcess pactl;
    pactl.start("pactl", {"load-module", "module-null-sink",
        "sink_name=AetherSDR-Output",
        "sink_properties=device.description=\"AetherSDR Output\"",
        "rate=24000", "channels=2", "format=float32le"});
    pactl.waitForFinished(3000);
    if (pactl.exitCode() != 0) {
        qWarning() << "DaxAudioManager: pactl failed for TX" << pactl.readAllStandardError();
        return;
    }
    m_impl->tx.moduleId = pactl.readAllStandardOutput().trimmed().toUInt();

    // Don't use pw_stream for TX capture — instead, manually link
    // the null sink's monitor to our capture stream after a short delay.
    // Create the PW stream without AUTOCONNECT, then link manually.
    auto* args = new Impl::CreateStreamArgs{
        m_impl.get(), nullptr, &m_impl->tx,
        "",  // no target — we'll link manually
        "AetherSDR-Output_capture",
        true  // INPUT direction (capture)
    };
    pw_loop_invoke(pw_main_loop_get_loop(m_impl->loop),
                   Impl::createStreamOnPwThread, 0, nullptr, 0, false, args);

    // Give PipeWire a moment to register the stream, then link manually
    QTimer::singleShot(500, this, [this]() {
        // Disconnect any auto-connected links first
        QProcess::execute("pw-link", {"-d", "AetherSDR:output_AUX0", "AetherSDR-Output_capture:input_FL"});
        QProcess::execute("pw-link", {"-d", "AetherSDR:output_AUX1", "AetherSDR-Output_capture:input_FR"});
        // Connect the null sink's monitor to our capture stream
        QProcess::execute("pw-link", {"AetherSDR-Output:monitor_FL", "AetherSDR-Output_capture:input_FL"});
        QProcess::execute("pw-link", {"AetherSDR-Output:monitor_FR", "AetherSDR-Output_capture:input_FR"});
        qDebug() << "DaxAudioManager: manually linked AetherSDR-Output monitor → capture";
    });

    m_impl->tx.ring.reset();
    m_impl->tx.active = true;
    qDebug() << "DaxAudioManager: activated TX sink, moduleId:" << m_impl->tx.moduleId;
    emit txStateChanged(true);
}

void DaxAudioManager::deactivateTx()
{
    if (!m_impl->tx.active) return;

    if (m_impl->tx.stream && m_impl->loop) {
        pw_loop_invoke(pw_main_loop_get_loop(m_impl->loop),
                       Impl::destroyStreamOnPwThread, 0, nullptr, 0, false, m_impl->tx.stream);
        m_impl->tx.stream = nullptr;
    }
    if (m_impl->tx.moduleId > 0) {
        QProcess::execute("pactl", {"unload-module", QString::number(m_impl->tx.moduleId)});
        m_impl->tx.moduleId = 0;
    }
    m_impl->tx.active = false;
    m_impl->tx.ring.reset();
    qDebug() << "DaxAudioManager: deactivated TX virtual sink";
    emit txStateChanged(false);
}

bool DaxAudioManager::isTxActive() const
{
    return m_impl->tx.active;
}

} // namespace AetherSDR

#else  // !HAVE_PIPEWIRE — stub implementation

namespace AetherSDR {

struct DaxAudioManager::Impl {};

DaxAudioManager::DaxAudioManager(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>()) {}
DaxAudioManager::~DaxAudioManager() = default;

bool DaxAudioManager::init() {
    qDebug() << "DaxAudioManager: PipeWire not available (compiled without HAVE_PIPEWIRE)";
    return false;
}
void DaxAudioManager::shutdown() {}
bool DaxAudioManager::isAvailable() const { return false; }
void DaxAudioManager::feedDaxRx(int, const QByteArray&) {}
QByteArray DaxAudioManager::readDaxTx(int) { return {}; }
void DaxAudioManager::activateRxChannel(int) {}
void DaxAudioManager::deactivateRxChannel(int) {}
bool DaxAudioManager::isRxChannelActive(int) const { return false; }
void DaxAudioManager::activateTx() {}
void DaxAudioManager::deactivateTx() {}
bool DaxAudioManager::isTxActive() const { return false; }

} // namespace AetherSDR

#endif // HAVE_PIPEWIRE
