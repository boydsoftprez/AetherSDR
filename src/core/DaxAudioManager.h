#pragma once

#include <QObject>
#include <QByteArray>
#include <array>
#include <memory>

namespace AetherSDR {

// Manages PipeWire virtual audio devices for DAX audio channels.
// Creates up to 4 RX virtual sources and 1 TX virtual sink.
// Compile-time optional: only available when HAVE_PIPEWIRE is defined.
class DaxAudioManager : public QObject {
    Q_OBJECT

public:
    explicit DaxAudioManager(QObject* parent = nullptr);
    ~DaxAudioManager() override;

    // Initialize PipeWire. Returns true if successful.
    bool init();

    // Shut down PipeWire, destroy all streams.
    void shutdown();

    // True if PipeWire initialized successfully.
    bool isAvailable() const;

    // Feed decoded RX audio (int16 stereo LE) to a DAX channel (1-4).
    void feedDaxRx(int channel, const QByteArray& pcm);

    // Read accumulated TX audio from the DAX TX virtual sink.
    // Returns float32 stereo LE data, up to maxBytes.
    QByteArray readDaxTx(int maxBytes);

    // Activate/deactivate individual DAX RX channels (1-4).
    void activateRxChannel(int channel);
    void deactivateRxChannel(int channel);
    bool isRxChannelActive(int channel) const;

    // Activate/deactivate the DAX TX virtual sink.
    void activateTx();
    void deactivateTx();
    bool isTxActive() const;

signals:
    // Emitted when TX ringbuffer has enough data for a VITA-49 packet.
    void daxTxDataReady();

    // Emitted when a channel's active state changes.
    void channelStateChanged(int channel, bool active);
    void txStateChanged(bool active);

    // RMS level for DAX RX channel (0.0–1.0), emitted per audio packet.
    void daxRxLevel(int channel, float rms);
    // RMS level for DAX TX (0.0–1.0).
    void daxTxLevel(float rms);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace AetherSDR
