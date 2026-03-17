#pragma once

#include <QWidget>
#include <array>

class QPushButton;
class QLabel;
class QLineEdit;
class QComboBox;
class QSlider;
class QProgressBar;

namespace AetherSDR {

class RadioModel;
class RigctlServer;
class RigctlPty;
class AudioEngine;
class DaxAudioManager;

// CAT Applet — settings panel for rigctld TCP server, virtual serial port,
// and DAX audio channel management.
class CatApplet : public QWidget {
    Q_OBJECT

public:
    explicit CatApplet(QWidget* parent = nullptr);

    void setRadioModel(RadioModel* model);
    void setRigctlServer(RigctlServer* server);
    void setRigctlPty(RigctlPty* pty);
    void setAudioEngine(AudioEngine* audio);
    void setDaxManager(DaxAudioManager* dax);

    // Sync Enable button state (called by MainWindow on autostart)
    void setTcpEnabled(bool on);
    void setPtyEnabled(bool on);

    // Update DAX channel status indicators
    void updateDaxChannelStatus(int channel, bool active);
    void updateDaxTxStatus(bool active);
    void setDaxSliceAssignment(int channel, const QString& sliceLetter);

signals:
    void daxEnableChanged(bool on);

private:
    void buildUI();
    void updateTcpStatus();
    void updatePtyStatus();
    void onConnectionStateChanged(bool connected);

    RadioModel*       m_model{nullptr};
    RigctlServer*     m_server{nullptr};
    RigctlPty*        m_pty{nullptr};
    AudioEngine*      m_audio{nullptr};
    DaxAudioManager*  m_daxManager{nullptr};

    // TCP section
    QPushButton* m_tcpEnable{nullptr};
    QLineEdit*   m_tcpPort{nullptr};
    QLabel*      m_tcpStatus{nullptr};

    // PTY section
    QPushButton* m_ptyEnable{nullptr};
    QLineEdit*   m_ptyPath{nullptr};

    // Slice selector
    QComboBox*   m_sliceSelect{nullptr};

    // DAX section
    QPushButton* m_daxEnable{nullptr};
    std::array<QLabel*, 4> m_daxStatus{};
    std::array<QLabel*, 4> m_daxIndicator{};
    std::array<QProgressBar*, 4> m_daxLevel{};
    QLabel*        m_daxTxStatus{nullptr};
    QLabel*        m_daxTxIndicator{nullptr};
    QProgressBar*  m_daxTxLevel{nullptr};
};

} // namespace AetherSDR
