#pragma once

#include "core/RadioDiscovery.h"
#include "core/SmartLinkClient.h"

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QTcpSocket>

namespace AetherSDR {

// Panel that shows discovered radios and a Connect/Disconnect button.
class ConnectionPanel : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionPanel(QWidget* parent = nullptr);

    void setConnected(bool connected);
    void setStatusText(const QString& text);
    void setCollapsed(bool collapsed);
    void probeRadio(const QString& ip);
    bool isCollapsed() const { return m_collapsed; }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

public slots:
    void onRadioDiscovered(const RadioInfo& radio);
    void onRadioUpdated(const RadioInfo& radio);
    void onRadioLost(const QString& serial);

    // SmartLink
    void setSmartLinkClient(SmartLinkClient* client);

signals:
    void connectRequested(const RadioInfo& radio);
    void wanConnectRequested(const WanRadioInfo& radio);
    void disconnectRequested();
    void collapsedChanged(bool collapsed);
    void routedRadioFound(const RadioInfo& radio);
    void smartLinkLoginRequested(const QString& email, const QString& password);

private slots:
    void onConnectClicked();
    void onListSelectionChanged();

private:

    QListWidget* m_radioList;
    QPushButton* m_connectBtn;
    QPushButton* m_collapseBtn;
    QLabel*      m_statusLabel;
    QLabel*      m_indicatorLabel;
    QWidget*     m_radioGroup;       // "Discovered Radios" group box

    QList<RadioInfo> m_radios;   // mirror of what's in the list
    bool m_connected{false};
    bool m_collapsed{false};
    int  m_expandedWidth{260};

    // SmartLink UI
    SmartLinkClient* m_smartLink{nullptr};
    QWidget*     m_smartLinkGroup{nullptr};
    QWidget*     m_loginForm{nullptr};
    QLineEdit*   m_emailEdit{nullptr};
    QLineEdit*   m_passwordEdit{nullptr};
    QPushButton* m_loginBtn{nullptr};
    QLabel*      m_slUserLabel{nullptr};
    QList<WanRadioInfo> m_wanRadios;

    // Manual (routed) connection
    QWidget*     m_manualGroup{nullptr};
    QLineEdit*   m_manualIpEdit{nullptr};
    QPushButton* m_manualProbeBtn{nullptr};
};

} // namespace AetherSDR
