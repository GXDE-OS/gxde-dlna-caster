#pragma once
#include <QObject>
#include <QNetworkAccessManager>

// UPnP SOAP 控制点: 向设备的 AVTransport 服务发送指令
class AvControl : public QObject
{
    Q_OBJECT
public:
    explicit AvControl(QObject *parent = nullptr);

    // 依次发送 SetAVTransportURI -> Play, 全部成功后发出 started()
    // mime 为流内容类型, 如 video/mp2t / audio/mpeg / image/jpeg
    void startCasting(const QString &controlUrl, const QString &uri,
                      const QString &mime = QStringLiteral("video/mp2t"));
    void stop(const QString &controlUrl);

signals:
    void started();                       // SetAVTransportURI + Play 均成功
    void commandResult(bool ok, const QString &error);

private:
    void post(const QString &controlUrl, const QString &action,
              const QStringList &pairs);

    QNetworkAccessManager m_nam;
    QString m_controlUrl;
    bool m_chainSet = false;   // SetAVTransportURI 已发送, 等待成功后发 Play
    bool m_chainPlay = false;  // Play 已发送, 等待成功后发出 started()
};
