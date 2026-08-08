#include "avcontrol.h"
#include <QNetworkReply>

namespace {
const QString kSoapEnv = QStringLiteral("http://schemas.xmlsoap.org/soap/envelope/");
const QString kAvTransport = QStringLiteral("urn:schemas-upnp-org:service:AVTransport:1");
}

AvControl::AvControl(QObject *parent)
    : QObject(parent)
{
    connect(&m_nam, &QNetworkAccessManager::finished, this,
            [this](QNetworkReply *reply) {
                reply->deleteLater();
                const bool ok = (reply->error() == QNetworkReply::NoError);
                if (!ok) {
                    m_chainSet = m_chainPlay = false;
                    emit commandResult(false, reply->errorString());
                    return;
                }
                if (m_chainSet) {
                    // SetAVTransportURI 成功, 继续发送 Play
                    m_chainSet = false;
                    post(m_controlUrl, "Play", QStringList()
                         << "InstanceID=0" << "Speed=1");
                    emit commandResult(true, QString());
                    return;
                }
                if (m_chainPlay) {
                    m_chainPlay = false;
                    emit started();
                }
                emit commandResult(true, QString());
            });
}

void AvControl::post(const QString &controlUrl, const QString &action,
                     const QStringList &pairs)
{
    QString argsXml;
    for (const QString &pair : pairs) {
        const int eq = pair.indexOf('=');
        QString v = pair.mid(eq + 1);
        v.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
        argsXml += QStringLiteral("<%1>%2</%1>").arg(pair.left(eq), v);
    }

    const QString body = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<s:Envelope xmlns:s=\"%1\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%2 xmlns:u=\"%3\">%4</u:%2></s:Body></s:Envelope>")
                             .arg(kSoapEnv, action, kAvTransport, argsXml);

    QNetworkRequest req{QUrl(controlUrl)};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "text/xml; charset=\"utf-8\"");
    req.setRawHeader("SOAPAction",
                     QStringLiteral("\"%1#%2\"").arg(kAvTransport, action).toUtf8());
    req.setRawHeader("User-Agent", "DLNA-Caster/1.0");
    req.setRawHeader("Connection", "close");
    m_nam.post(req, body.toUtf8());
}

void AvControl::startCasting(const QString &controlUrl, const QString &uri)
{
    m_controlUrl = controlUrl;
    m_chainSet = true;
    m_chainPlay = true;

    const QString didl = QStringLiteral(
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
        "<item id=\"0\" parentID=\"-1\" restricted=\"1\">"
        "<dc:title>DLNA-Caster 投屏</dc:title>"
        "<res protocolInfo=\"http-get:*:video/mp2t:*\">%1</res>"
        "</item></DIDL-Lite>").arg(uri);

    post(m_controlUrl, "SetAVTransportURI", QStringList()
         << "InstanceID=0"
         << QStringLiteral("CurrentURI=%1").arg(uri)
         << QStringLiteral("CurrentURIMetaData=%1").arg(didl));
}

void AvControl::stop(const QString &controlUrl)
{
    m_chainSet = m_chainPlay = false;
    post(controlUrl, "Stop", QStringList() << "InstanceID=0");
}
