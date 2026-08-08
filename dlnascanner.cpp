#include "dlnascanner.h"
#include <QNetworkReply>
#include <QNetworkInterface>
#include <QNetworkAddressEntry>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

static const QHostAddress kSsdpAddr = QHostAddress(QStringLiteral("239.255.255.250"));
static constexpr quint16 kSsdpPort = 1900;

// 精确搜索 MediaRenderer
static const QByteArray kSearchMediaRenderer =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
    "USER-AGENT: DLNA-Caster/1.0\r\n"
    "\r\n";

// 兜底搜索全部 UPnP 设备 (兼容 MediaRenderer:2 / 厂商自定义类型, 靠 XML 过滤)
static const QByteArray kSearchAll =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: ssdp:all\r\n"
    "USER-AGENT: DLNA-Caster/1.0\r\n"
    "\r\n";

DlnaScanner::DlnaScanner(QObject *parent)
    : QObject(parent)
{
    connect(&m_nam, &QNetworkAccessManager::finished,
            this, &DlnaScanner::onReplyFinished);
}

void DlnaScanner::startDiscovery(int timeoutMs)
{
    m_collectMs = timeoutMs;
    m_locations.clear();
    m_fetching.clear();

    // 清理上一次扫描的 socket
    for (QUdpSocket *s : qAsConst(m_socks)) {
        s->disconnect(this);
        s->deleteLater();
    }
    m_socks.clear();

    // 向每个可用 IPv4 网卡都发送一次 SSDP 探测, 避免多网卡/默认路由被
    // VPN 等占用时组播发错接口而找不到设备
    QStringList used;
    const QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp))
            continue;
        if (iface.flags() & QNetworkInterface::IsLoopBack)
            continue;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            auto *s = new QUdpSocket(this);
            if (!s->bind(ip, 0, QUdpSocket::ShareAddress)) {
                s->deleteLater();
                continue;
            }
            connect(s, &QUdpSocket::readyRead, this, &DlnaScanner::onUdpReady);
            s->writeDatagram(kSearchMediaRenderer, kSsdpAddr, kSsdpPort);
            s->writeDatagram(kSearchAll, kSsdpAddr, kSsdpPort);
            m_socks << s;
            used << ip.toString();
        }
    }
    emit interfacesUsed(used);

    QTimer::singleShot(m_collectMs, this, &DlnaScanner::onCollectTimeout);
}

void DlnaScanner::onUdpReady()
{
    for (QUdpSocket *sock : qAsConst(m_socks)) {
        while (sock->hasPendingDatagrams()) {
            QByteArray data;
            data.resize(int(sock->pendingDatagramSize()));
            sock->readDatagram(data.data(), data.size());
            const QList<QByteArray> lines = data.split('\n');
            for (const QByteArray &line : lines) {
                const int idx = line.indexOf(':');
                if (idx <= 0)
                    continue;
                if (line.left(idx).trimmed().toLower() != "location")
                    continue;
                const QString loc = QString::fromUtf8(line.mid(idx + 1).trimmed());
                if (!loc.isEmpty())
                    m_locations.insert(loc);
            }
        }
    }
}

void DlnaScanner::onCollectTimeout()
{
    startFetching();
}

void DlnaScanner::startFetching()
{
    if (m_locations.isEmpty()) {
        emit finished();
        return;
    }
    for (const QString &loc : m_locations) {
        if (m_fetching.contains(loc))
            continue;
        m_fetching.insert(loc);
        QNetworkRequest req{QUrl(loc)};
        req.setRawHeader("User-Agent", "DLNA-Caster/1.0");
        m_nam.get(req);
    }
    if (m_fetching.isEmpty())
        emit finished();
}

void DlnaScanner::onReplyFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    m_fetching.remove(reply->url().toString());
    if (reply->error() == QNetworkReply::NoError) {
        const Renderer r = parseDeviceXml(reply->readAll(), reply->url());
        if (r.valid())
            emit deviceFound(r);
    }
    if (m_fetching.isEmpty())
        emit finished();
}

Renderer DlnaScanner::parseDeviceXml(const QByteArray &xml, const QUrl &location)
{
    Renderer r;
    QXmlStreamReader reader(xml);
    QString current;
    bool inDevice = false;
    bool inService = false;
    QString deviceType, friendlyName, udn;
    QString serviceType, controlUrl, eventUrl;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            current = reader.name().toString();
            if (current == "device")
                inDevice = true;
            else if (current == "service")
                inService = true;
            continue;
        }
        if (reader.isCharacters() && !reader.isWhitespace()) {
            const QString text = reader.text().toString().trimmed();
            if (inService) {
                if (current == "serviceType")
                    serviceType = text;
                else if (current == "controlURL" && serviceType.contains("AVTransport"))
                    controlUrl = text;
                else if (current == "eventSubURL" && serviceType.contains("AVTransport"))
                    eventUrl = text;
            } else if (inDevice) {
                if (current == "deviceType")
                    deviceType = text;
                else if (current == "friendlyName")
                    friendlyName = text;
                else if (current == "UDN")
                    udn = text;
            }
            continue;
        }
        if (reader.isEndElement()) {
            const QString name = reader.name().toString();
            if (name == "service")
                inService = false;
            else if (name == "device")
                inDevice = false;
            current.clear();
        }
    }

    if (!deviceType.contains("MediaRenderer") || controlUrl.isEmpty())
        return r;

    r.name = friendlyName.isEmpty() ? location.toString() : friendlyName;
    r.location = location.toString();
    r.controlUrl = location.resolved(QUrl(controlUrl)).toString();
    r.eventUrl = eventUrl.isEmpty() ? QString()
                                    : location.resolved(QUrl(eventUrl)).toString();
    r.udn = udn;
    return r;
}
