#include "AppletPanel.h"
#include "RxApplet.h"

#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>

namespace AetherSDR {

// ── Placeholder applet widget ────────────────────────────────────────────────

static QWidget* makePlaceholder(const QString& name)
{
    // Each placeholder has the same gradient title bar style as RxApplet
    // so it integrates visually when revealed.
    auto* w = new QWidget;
    w->hide();

    auto* outer = new QVBoxLayout(w);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Title bar
    auto* bar = new QWidget;
    bar->setFixedHeight(16);
    bar->setStyleSheet(
        "QWidget { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "stop:0 #3a4a5a, stop:0.5 #2a3a4a, stop:1 #1a2a38); "
        "border-bottom: 1px solid #0a1a28; }");
    auto* lbl = new QLabel(name, bar);
    lbl->setStyleSheet("QLabel { background: transparent; color: #8aa8c0; "
                       "font-size: 10px; font-weight: bold; }");
    lbl->setGeometry(6, 1, 200, 14);
    outer->addWidget(bar);

    // Body
    auto* body = new QWidget;
    auto* bl = new QVBoxLayout(body);
    auto* txt = new QLabel(name + " applet\n(coming soon)");
    txt->setAlignment(Qt::AlignCenter);
    txt->setStyleSheet("color: #405060; font-size: 11px;");
    txt->setFixedHeight(40);
    bl->addWidget(txt);
    outer->addWidget(body);

    return w;
}

// ── AppletPanel ──────────────────────────────────────────────────────────────

AppletPanel::AppletPanel(QWidget* parent) : QWidget(parent)
{
    setFixedWidth(260);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Toggle button row ─────────────────────────────────────────────────────
    auto* btnRow = new QWidget;
    btnRow->setStyleSheet(
        "QWidget { background: #0a0a18; border-bottom: 1px solid #1e2e3e; }"
        "QPushButton { background: #1a2a3a; border: 1px solid #203040; "
        "border-radius: 3px; padding: 2px 5px; font-size: 11px; color: #c8d8e8; }"
        "QPushButton:checked { background: #0070c0; color: #ffffff; "
        "border: 1px solid #0090e0; }");
    auto* btnLayout = new QHBoxLayout(btnRow);
    btnLayout->setContentsMargins(3, 3, 3, 3);
    btnLayout->setSpacing(2);
    root->addWidget(btnRow);

    // ── Scrollable applet stack ───────────────────────────────────────────────
    auto* scrollArea = new QScrollArea;
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setWidgetResizable(true);

    auto* container = new QWidget;
    m_stack = new QVBoxLayout(container);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->setSpacing(0);
    m_stack->addStretch();
    scrollArea->setWidget(container);
    root->addWidget(scrollArea, 1);

    // ── Helper: add one applet with its toggle button ─────────────────────────
    auto addApplet = [&](const QString& label, QWidget* applet) {
        auto* btn = new QPushButton(label, btnRow);
        btn->setCheckable(true);
        btnLayout->addWidget(btn);

        // Insert before the trailing stretch (index = count - 1).
        m_stack->insertWidget(m_stack->count() - 1, applet);

        connect(btn, &QPushButton::toggled, applet, &QWidget::setVisible);
    };

    // RX applet — visible by default
    m_rxApplet = new RxApplet;
    addApplet("RX", m_rxApplet);
    btnLayout->itemAt(0)->widget()->setProperty("checked", true);  // visual only
    // Actually check the button properly:
    static_cast<QPushButton*>(btnLayout->itemAt(0)->widget())->setChecked(true);
    m_rxApplet->show();

    // Placeholder applets — hidden by default
    addApplet("TX",   makePlaceholder("TX"));
    addApplet("P/CW", makePlaceholder("P/CW"));
    addApplet("PHNE", makePlaceholder("PHNE"));
    addApplet("EQ",   makePlaceholder("EQ"));
    addApplet("ANLG", makePlaceholder("ANLG"));

    btnLayout->addStretch();
}

void AppletPanel::setSlice(SliceModel* slice)
{
    m_rxApplet->setSlice(slice);
}

void AppletPanel::setAntennaList(const QStringList& ants)
{
    m_rxApplet->setAntennaList(ants);
}

} // namespace AetherSDR
