#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QList>
#include <QMap>
#include <QString>
#include <QHostAddress>

namespace AetherSDR {

// Represents a discovered FlexRadio on the network.
struct RadioInfo {
    QString name;           // e.g. "FLEX-6600"
    QString model;
    QString serial;
    QString version;
    QString nickname;
    QString callsign;
    QHostAddress address;
    quint16 port{4992};
    QString status;         // "Available" | "In_Use" | etc.
    int maxLicensedVersion{0};
    bool inUse{false};
    bool isRouted{false};

    // Connected GUI clients (comma-separated in discovery packet)
    QStringList guiClientPrograms;   // e.g. "SmartSDR-Win", "Maestro"
    QStringList guiClientStations;   // e.g. "FLEX_NUC_WX7Y"
    QStringList guiClientHandles;    // e.g. "0x7F1C30C0"

    bool hasOtherClients() const { return !guiClientPrograms.isEmpty(); }

    QString displayName() const {
        const QString connType = isRouted ? "routed" : "local";
        QString suffix;
        if (!guiClientPrograms.isEmpty())
            suffix = QString("multiFLEX: %1").arg(guiClientPrograms.first());
        else
            suffix = connType;
        if (nickname.isEmpty() && callsign.isEmpty())
            return QString("%1 @ %2\nAvailable (%3)")
                .arg(model, address.toString(), suffix);
        return QString("%1  %2  %3\nAvailable (%4)")
            .arg(model, nickname, callsign, suffix);
    }
};

// Listens for SmartSDR discovery broadcasts on UDP port 4992
// and emits radioDiscovered / radioLost signals as radios appear/disappear.
class RadioDiscovery : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 DISCOVERY_PORT = 4992;
    static constexpr int STALE_TIMEOUT_MS  = 10000; // radio considered gone after 10s

    explicit RadioDiscovery(QObject* parent = nullptr);
    ~RadioDiscovery() override;

    void startListening();
    void stopListening();

    QList<RadioInfo> discoveredRadios() const { return m_radios; }

signals:
    void radioDiscovered(const RadioInfo& radio);
    void radioUpdated(const RadioInfo& radio);
    void radioLost(const QString& serial);

private slots:
    void onReadyRead();
    void onStaleCheck();

private:
    RadioInfo parseDiscoveryPacket(const QByteArray& data) const;
    void upsertRadio(const RadioInfo& info);

    QUdpSocket        m_socket;
    QTimer            m_staleTimer;
    QList<RadioInfo>  m_radios;

    // Track last-seen time per serial for staleness detection
    QMap<QString, qint64> m_lastSeen;
};

} // namespace AetherSDR
