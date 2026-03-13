#pragma once

#include <QWidget>
#include <QStringList>

class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace AetherSDR {

class SliceModel;
class RxApplet;

// AppletPanel — right-side panel with a row of toggle buttons at the top.
// Each button shows/hides its corresponding applet in the scrollable stack
// below.  Multiple applets can be visible simultaneously.
class AppletPanel : public QWidget {
    Q_OBJECT

public:
    explicit AppletPanel(QWidget* parent = nullptr);

    void setSlice(SliceModel* slice);
    void setAntennaList(const QStringList& ants);

    RxApplet* rxApplet() { return m_rxApplet; }

private:
    RxApplet*    m_rxApplet{nullptr};
    QVBoxLayout* m_stack{nullptr};   // layout inside the scroll area
};

} // namespace AetherSDR
