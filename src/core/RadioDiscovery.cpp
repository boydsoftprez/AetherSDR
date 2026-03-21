#include "RadioDiscovery.h"
#include "LogManager.h"

#include <QNetworkDatagram>
#include <QDateTime>

namespace AetherSDR {

RadioDiscovery::RadioDiscovery(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &RadioDiscovery::onReadyRead);
    m_staleTimer.setInterval(STALE_TIMEOUT_MS / 2);
    connect(&m_staleTimer, &QTimer::timeout, this, &RadioDiscovery::onStaleCheck);
}

RadioDiscovery::~RadioDiscovery()
{
    stopListening();
}

void RadioDiscovery::startListening()
{
    if (!m_socket.bind(QHostAddress::AnyIPv4, DISCOVERY_PORT,
                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qCWarning(lcDiscovery) << "RadioDiscovery: failed to bind UDP port" << DISCOVERY_PORT
                   << m_socket.errorString();
        return;
    }
    m_staleTimer.start();
    qCDebug(lcDiscovery) << "RadioDiscovery: listening on UDP" << DISCOVERY_PORT;
}

void RadioDiscovery::stopListening()
{
    m_staleTimer.stop();
    m_socket.close();
}

// SmartSDR discovery packets are ASCII key=value pairs separated by spaces.
// Example:
//   name=my-flex model=FLEX-6600 serial=1234ABCD version=3.3.28.0
//   ip=192.168.1.50 port=4992 status=Available max_licensed_version=3
RadioInfo RadioDiscovery::parseDiscoveryPacket(const QByteArray& data) const
{
    RadioInfo info;
    const QString text = QString::fromUtf8(data).trimmed();

    for (const QString& token : text.split(' ', Qt::SkipEmptyParts)) {
        const int eq = token.indexOf('=');
        if (eq < 0) continue;

        const QString key   = token.left(eq).toLower();
        const QString value = token.mid(eq + 1);

        if      (key == "name")    info.name    = value;
        else if (key == "model")   info.model   = value;
        else if (key == "serial")  info.serial  = value;
        else if (key == "version") info.version = value;
        else if (key == "ip")      info.address = QHostAddress(value);
        else if (key == "port")    info.port    = value.toUShort();
        else if (key == "status")  info.status  = value;
        else if (key == "nickname") info.nickname = value;
        else if (key == "callsign") info.callsign = value;
        else if (key == "inuse")   info.inUse   = (value == "1");
        else if (key == "max_licensed_version") info.maxLicensedVersion = value.toInt();
        else if (key == "gui_client_programs")
            info.guiClientPrograms = value.split(',', Qt::SkipEmptyParts);
        else if (key == "gui_client_stations") {
            // Stations use \x7f for spaces
            QString cleaned = value;
            cleaned.replace('\x7f', ' ');
            info.guiClientStations = cleaned.split(',', Qt::SkipEmptyParts);
        }
        else if (key == "gui_client_handles")
            info.guiClientHandles = value.split(',', Qt::SkipEmptyParts);
    }
    return info;
}

void RadioDiscovery::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_socket.receiveDatagram();
        if (datagram.isNull()) continue;

        RadioInfo info = parseDiscoveryPacket(datagram.data());

        // Fall back to sender address if the packet didn't include an IP field
        if (info.address.isNull())
            info.address = datagram.senderAddress();

        if (info.serial.isEmpty()) {
            qCWarning(lcDiscovery) << "RadioDiscovery: received packet without serial, ignoring";
            continue;
        }

        m_lastSeen[info.serial] = QDateTime::currentMSecsSinceEpoch();
        upsertRadio(info);
    }
}

void RadioDiscovery::upsertRadio(const RadioInfo& info)
{
    for (int i = 0; i < m_radios.size(); ++i) {
        if (m_radios[i].serial == info.serial) {
            m_radios[i] = info;
            emit radioUpdated(info);
            return;
        }
    }
    m_radios.append(info);
    qCDebug(lcDiscovery) << "RadioDiscovery: found" << info.displayName();
    emit radioDiscovered(info);
}

void RadioDiscovery::onStaleCheck()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList lost;

    for (auto it = m_lastSeen.cbegin(); it != m_lastSeen.cend(); ++it) {
        if (now - it.value() > STALE_TIMEOUT_MS)
            lost.append(it.key());
    }

    for (const QString& serial : lost) {
        m_lastSeen.remove(serial);
        m_radios.removeIf([&](const RadioInfo& r){ return r.serial == serial; });
        qCDebug(lcDiscovery) << "RadioDiscovery: lost radio" << serial;
        emit radioLost(serial);
    }
}

} // namespace AetherSDR
