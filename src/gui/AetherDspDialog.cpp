#include "AetherDspDialog.h"
#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "GuardedSlider.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QRadioButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>

namespace AetherSDR {

static const QString kDialogStyle = QStringLiteral(
    "QDialog { background: #0f0f1a; color: #c8d8e8; }"
    "QTabWidget::pane { border: 1px solid #304050; background: #0f0f1a; }"
    "QTabBar::tab { background: #1a2a3a; color: #8090a0; padding: 6px 16px;"
    "  border: 1px solid #304050; border-bottom: none; border-radius: 3px 3px 0 0; }"
    "QTabBar::tab:selected { background: #0f0f1a; color: #c8d8e8;"
    "  border-bottom: 1px solid #0f0f1a; }"
    "QGroupBox { border: 1px solid #304050; border-radius: 4px;"
    "  margin-top: 12px; padding-top: 8px; color: #8090a0; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
    "QLabel { color: #8090a0; }"
    "QRadioButton { color: #c8d8e8; }"
    "QCheckBox { color: #c8d8e8; }"
    "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050;"
    "  border-radius: 3px; padding: 4px 12px; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); border: 1px solid #0090e0; }");

static const QString kSliderStyle = QStringLiteral(
    "QSlider::groove:horizontal { height: 4px; background: #304050; border-radius: 2px; }"
    "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0;"
    "  background: #c8d8e8; border-radius: 6px; }"
    "QSlider::handle:horizontal:hover { background: #00b4d8; }");

AetherDspDialog::AetherDspDialog(AudioEngine* audio, QWidget* parent)
    : QDialog(parent)
    , m_audio(audio)
{
    setWindowTitle("AetherDSP Settings");
    setMinimumSize(420, 380);
    setStyleSheet(kDialogStyle);

    auto* root = new QVBoxLayout(this);

    auto* title = new QLabel("AetherDSP Settings");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: #c8d8e8; }");
    root->addWidget(title);
    root->addSpacing(4);

    auto* tabs = new QTabWidget;
    buildNr2Tab(tabs);
    buildNr4Tab(tabs);
    buildRn2Tab(tabs);
    buildBnrTab(tabs);
    root->addWidget(tabs);

    syncFromEngine();
}

// ── NR2 Tab ──────────────────────────────────────────────────────────────────

void AetherDspDialog::buildNr2Tab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);

    auto labelStyle = QStringLiteral("QLabel { color: #8090a0; font-size: 11px; }");
    auto valStyle   = QStringLiteral("QLabel { color: #c8d8e8; font-size: 11px; min-width: 40px; }");

    // Gain Method
    auto* gainGroup = new QGroupBox("Gain Method");
    auto* gainLayout = new QHBoxLayout(gainGroup);
    m_nr2GainGroup = new QButtonGroup(this);
    const char* gainLabels[] = {"Linear", "Log", "Gamma", "Trained"};
    for (int i = 0; i < 4; ++i) {
        auto* rb = new QRadioButton(gainLabels[i]);
        m_nr2GainGroup->addButton(rb, i);
        gainLayout->addWidget(rb);
    }
    m_nr2GainGroup->button(2)->setChecked(true);  // Gamma default
    connect(m_nr2GainGroup, &QButtonGroup::idClicked, this, [this](int id) {
        auto& s = AppSettings::instance();
        s.setValue("NR2GainMethod", QString::number(id));
        s.save();
        emit nr2GainMethodChanged(id);
    });
    vbox->addWidget(gainGroup);

    // NPE Method
    auto* npeGroup = new QGroupBox("NPE Method");
    auto* npeLayout = new QHBoxLayout(npeGroup);
    m_nr2NpeGroup = new QButtonGroup(this);
    const char* npeLabels[] = {"OSMS", "MMSE", "NSTAT"};
    for (int i = 0; i < 3; ++i) {
        auto* rb = new QRadioButton(npeLabels[i]);
        m_nr2NpeGroup->addButton(rb, i);
        npeLayout->addWidget(rb);
    }
    m_nr2NpeGroup->button(0)->setChecked(true);  // OSMS default
    connect(m_nr2NpeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        auto& s = AppSettings::instance();
        s.setValue("NR2NpeMethod", QString::number(id));
        s.save();
        emit nr2NpeMethodChanged(id);
    });
    vbox->addWidget(npeGroup);

    // AE Filter
    m_nr2AeCheck = new QCheckBox("AE Filter (artifact elimination)");
    m_nr2AeCheck->setChecked(true);
    connect(m_nr2AeCheck, &QCheckBox::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("NR2AeFilter", on ? "True" : "False");
        s.save();
        emit nr2AeFilterChanged(on);
    });
    vbox->addWidget(m_nr2AeCheck);

    // Sliders: GainMax, GainSmooth, Q_SPP
    auto* sliderGrid = new QGridLayout;
    int row = 0;

    // Gain Max (reduction depth)
    {
        auto* lbl = new QLabel("Reduction Depth:");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr2GainMaxSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2GainMaxSlider->setRange(50, 200);  // 0.50–2.00
        m_nr2GainMaxSlider->setValue(150);       // default 1.50
        m_nr2GainMaxSlider->setStyleSheet(kSliderStyle);
        sliderGrid->addWidget(m_nr2GainMaxSlider, row, 1);
        m_nr2GainMaxLabel = new QLabel("1.50");
        m_nr2GainMaxLabel->setStyleSheet(valStyle);
        m_nr2GainMaxLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2GainMaxLabel, row, 2);
        connect(m_nr2GainMaxSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr2GainMaxLabel->setText(QString::number(val, 'f', 2));
            auto& s = AppSettings::instance();
            s.setValue("NR2GainMax", QString::number(val, 'f', 2));
            s.save();
            emit nr2GainMaxChanged(val);
        });
        ++row;
    }

    // Gain Smooth
    {
        auto* lbl = new QLabel("Smoothing:");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr2SmoothSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2SmoothSlider->setRange(50, 98);  // 0.50–0.98
        m_nr2SmoothSlider->setValue(85);       // default 0.85
        m_nr2SmoothSlider->setStyleSheet(kSliderStyle);
        sliderGrid->addWidget(m_nr2SmoothSlider, row, 1);
        m_nr2SmoothLabel = new QLabel("0.85");
        m_nr2SmoothLabel->setStyleSheet(valStyle);
        m_nr2SmoothLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2SmoothLabel, row, 2);
        connect(m_nr2SmoothSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr2SmoothLabel->setText(QString::number(val, 'f', 2));
            auto& s = AppSettings::instance();
            s.setValue("NR2GainSmooth", QString::number(val, 'f', 2));
            s.save();
            emit nr2GainSmoothChanged(val);
        });
        ++row;
    }

    // Q_SPP (voice threshold)
    {
        auto* lbl = new QLabel("Voice Threshold:");
        lbl->setStyleSheet(labelStyle);
        sliderGrid->addWidget(lbl, row, 0);
        m_nr2QsppSlider = new GuardedSlider(Qt::Horizontal);
        m_nr2QsppSlider->setRange(5, 50);  // 0.05–0.50
        m_nr2QsppSlider->setValue(20);      // default 0.20
        m_nr2QsppSlider->setStyleSheet(kSliderStyle);
        sliderGrid->addWidget(m_nr2QsppSlider, row, 1);
        m_nr2QsppLabel = new QLabel("0.20");
        m_nr2QsppLabel->setStyleSheet(valStyle);
        m_nr2QsppLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sliderGrid->addWidget(m_nr2QsppLabel, row, 2);
        connect(m_nr2QsppSlider, &QSlider::valueChanged, this, [this](int v) {
            float val = v / 100.0f;
            m_nr2QsppLabel->setText(QString::number(val, 'f', 2));
            auto& s = AppSettings::instance();
            s.setValue("NR2Qspp", QString::number(val, 'f', 2));
            s.save();
            emit nr2QsppChanged(val);
        });
        ++row;
    }

    vbox->addLayout(sliderGrid);

    // Reset button
    auto* resetBtn = new QPushButton("Reset Defaults");
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        m_nr2GainGroup->button(2)->setChecked(true);    // Gamma
        m_nr2NpeGroup->button(0)->setChecked(true);     // OSMS
        m_nr2AeCheck->setChecked(true);
        m_nr2GainMaxSlider->setValue(150);               // 1.50
        m_nr2SmoothSlider->setValue(85);                 // 0.85
        m_nr2QsppSlider->setValue(20);                   // 0.20
    });
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(resetBtn);
    vbox->addLayout(btnRow);

    vbox->addStretch();
    tabs->addTab(page, "NR2");
}

// ── NR4 Tab (placeholder — Phase 2) ─────────────────────────────────────────

void AetherDspDialog::buildNr4Tab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    auto* lbl = new QLabel("NR4 (Spectral Bleach) — coming soon");
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setStyleSheet("QLabel { color: #506070; font-size: 14px; }");
    vbox->addWidget(lbl);
    vbox->addStretch();
    tabs->addTab(page, "NR4");
}

// ── RN2 Tab ─────────────────────────────────────────────────────────────────

void AetherDspDialog::buildRn2Tab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    auto* lbl = new QLabel("RN2 (RNNoise) — no adjustable parameters");
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setStyleSheet("QLabel { color: #506070; font-size: 14px; }");
    vbox->addWidget(lbl);
    vbox->addStretch();
    tabs->addTab(page, "RN2");
}

// ── BNR Tab ─────────────────────────────────────────────────────────────────

void AetherDspDialog::buildBnrTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* vbox = new QVBoxLayout(page);
    auto* lbl = new QLabel("BNR (NVIDIA) — intensity controlled from overlay menu");
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setStyleSheet("QLabel { color: #506070; font-size: 14px; }");
    vbox->addWidget(lbl);
    vbox->addStretch();
    tabs->addTab(page, "BNR");
}

// ── Sync from saved settings ─────────────────────────────────────────────────

void AetherDspDialog::syncFromEngine()
{
    auto& s = AppSettings::instance();

    int gainMethod = s.value("NR2GainMethod", "2").toInt();
    if (auto* btn = m_nr2GainGroup->button(gainMethod))
        btn->setChecked(true);

    int npeMethod = s.value("NR2NpeMethod", "0").toInt();
    if (auto* btn = m_nr2NpeGroup->button(npeMethod))
        btn->setChecked(true);

    bool aeFilter = s.value("NR2AeFilter", "True").toString() == "True";
    m_nr2AeCheck->setChecked(aeFilter);

    int gainMax = static_cast<int>(s.value("NR2GainMax", "1.50").toFloat() * 100);
    m_nr2GainMaxSlider->setValue(gainMax);
    m_nr2GainMaxLabel->setText(QString::number(gainMax / 100.0f, 'f', 2));

    int smooth = static_cast<int>(s.value("NR2GainSmooth", "0.85").toFloat() * 100);
    m_nr2SmoothSlider->setValue(smooth);
    m_nr2SmoothLabel->setText(QString::number(smooth / 100.0f, 'f', 2));

    int qspp = static_cast<int>(s.value("NR2Qspp", "0.20").toFloat() * 100);
    m_nr2QsppSlider->setValue(qspp);
    m_nr2QsppLabel->setText(QString::number(qspp / 100.0f, 'f', 2));
}

} // namespace AetherSDR
