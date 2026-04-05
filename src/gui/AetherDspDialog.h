#pragma once

#include <QDialog>
#include <QTabWidget>

class QSlider;
class QLabel;
class QPushButton;
class QRadioButton;
class QCheckBox;
class QButtonGroup;

namespace AetherSDR {

class AudioEngine;

// AetherDSP Settings — modeless dialog for client-side DSP parameters.
// Accessible from Settings menu and "AetherDSP Settings..." in right-click popups.
// All values persist via AppSettings (PascalCase keys).
class AetherDspDialog : public QDialog {
    Q_OBJECT

public:
    explicit AetherDspDialog(AudioEngine* audio, QWidget* parent = nullptr);

    // Sync UI from current AudioEngine state
    void syncFromEngine();

signals:
    // NR2 parameter changes
    void nr2GainMethodChanged(int method);
    void nr2NpeMethodChanged(int method);
    void nr2AeFilterChanged(bool on);
    void nr2GainMaxChanged(float value);
    void nr2GainSmoothChanged(float value);
    void nr2QsppChanged(float value);

private:
    void buildNr2Tab(QTabWidget* tabs);
    void buildNr4Tab(QTabWidget* tabs);
    void buildRn2Tab(QTabWidget* tabs);
    void buildBnrTab(QTabWidget* tabs);

    AudioEngine* m_audio;

    // NR2 controls
    QButtonGroup* m_nr2GainGroup{nullptr};
    QButtonGroup* m_nr2NpeGroup{nullptr};
    QCheckBox*    m_nr2AeCheck{nullptr};
    QSlider*      m_nr2GainMaxSlider{nullptr};
    QLabel*       m_nr2GainMaxLabel{nullptr};
    QSlider*      m_nr2SmoothSlider{nullptr};
    QLabel*       m_nr2SmoothLabel{nullptr};
    QSlider*      m_nr2QsppSlider{nullptr};
    QLabel*       m_nr2QsppLabel{nullptr};
};

} // namespace AetherSDR
