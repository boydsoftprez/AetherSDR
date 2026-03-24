#pragma once

#include <QWidget>

class QPushButton;
class QTextEdit;
class QSpinBox;
class QLabel;
class QLineEdit;
class QStackedWidget;

namespace AetherSDR {

class CwxModel;

class CwxPanel : public QWidget {
    Q_OBJECT
public:
    explicit CwxPanel(CwxModel* model, QWidget* parent = nullptr);

    void setModel(CwxModel* model);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onCharSent(int index);
    void onSpeedChanged(int wpm);

private:
    void buildSendView();
    void buildSetupView();
    void showSendView();
    void showSetupView();
    void sendBuffer();
    void onKeyPress(const QString& text);

    CwxModel*       m_model{nullptr};

    QStackedWidget* m_stack{nullptr};

    // Send/Live view
    QWidget*        m_sendPage{nullptr};
    QTextEdit*      m_textEdit{nullptr};
    int             m_sendStartIndex{0};  // cumulative index offset for highlighting

    // Setup view
    QWidget*        m_setupPage{nullptr};
    QLineEdit*      m_macroEdits[12]{};
    QSpinBox*       m_delaySpin{nullptr};
    QPushButton*    m_qskBtn{nullptr};

    // Bottom bar
    QPushButton*    m_sendBtn{nullptr};
    QPushButton*    m_liveBtn{nullptr};
    QPushButton*    m_setupBtn{nullptr};
    QSpinBox*       m_speedSpin{nullptr};
};

} // namespace AetherSDR
