#pragma once
#include <QObject>
#include <QUdpSocket>
#include <QNetworkAccessManager>
#include <QSet>
#include <QList>
#include "renderer.h"

// SSDP 发现局域网内的 DLNA 播放设备
class DlnaScanner : public QObject
{
    Q_OBJECT
public:
    explicit DlnaScanner(QObject *parent = nullptr);

    void startDiscovery(int timeoutMs = 6000);

signals:
    void deviceFound(const Renderer &device);
    void interfacesUsed(const QStringList &ips);  // 本次扫描实际使用的网卡 IP
    void finished();

private slots:
    void onUdpReady();
    void onReplyFinished(QNetworkReply *reply);
    void onCollectTimeout();

private:
    void startFetching();
    static Renderer parseDeviceXml(const QByteArray &xml, const QUrl &location);

    QList<QUdpSocket *> m_socks;
    QNetworkAccessManager m_nam;
    QSet<QString> m_locations;
    QSet<QString> m_fetching;
    int m_collectMs = 6000;
};
