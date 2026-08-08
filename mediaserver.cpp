#include "mediaserver.h"
#include <QTcpSocket>

MediaServer::MediaServer(QObject *parent)
    : QTcpServer(parent)
{
    connect(this, &QTcpServer::newConnection,
            this, &MediaServer::onNewConnection);
}

bool MediaServer::start(quint16 port)
{
    return listen(QHostAddress::Any, port);
}

void MediaServer::setStreamProcess(QProcess *proc)
{
    if (m_proc)
        disconnect(m_proc, nullptr, this, nullptr);
    m_proc = proc;
    if (!proc)
        return;
    connect(proc, &QProcess::readyReadStandardOutput,
            this, &MediaServer::onProcessReadyRead);
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &MediaServer::onProcessFinished);
}

void MediaServer::setStaticFile(const QByteArray &data, const QString &mime)
{
    m_staticData = data;
    m_staticMime = mime;
    m_staticMode = !data.isEmpty();
    setStreamProcess(nullptr);
}

void MediaServer::setStreamContentType(const QString &mime)
{
    m_streamMime = mime;
}

void MediaServer::stop()
{
    for (Client &c : m_clients) {
        if (c.streaming) {
            c.out += "0\r\n\r\n";
            pump(c);
        }
        c.sock->disconnectFromHost();
        c.sock->deleteLater();
    }
    m_clients.clear();
    close();
}

void MediaServer::onNewConnection()
{
    while (hasPendingConnections()) {
        QTcpSocket *sock = nextPendingConnection();
        if (!sock)
            continue;
        Client c;
        c.sock = sock;
        m_clients.insert(sock, c);
        connect(sock, &QTcpSocket::readyRead, this, &MediaServer::onClientReady);
        connect(sock, &QTcpSocket::disconnected,
                this, &MediaServer::onClientDisconnected);
        connect(sock, &QTcpSocket::bytesWritten,
                this, &MediaServer::onClientBytesWritten);
    }
}

void MediaServer::onClientReady()
{
    auto *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock || !m_clients.contains(sock))
        return;
    Client &c = m_clients[sock];
    c.in += sock->readAll();

    if (!c.headersSent) {
        const int end = c.in.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QByteArray head = c.in.left(end);
        const QByteArray firstLine = head.split('\n').value(0);
        c.in.remove(0, end + 4);
        c.headersSent = true;

        if (firstLine.contains("/stream")) {
            c.streaming = true;
            if (m_staticMode) {
                // 静态文件模式 (如图片): Content-Length 一次性返回
                c.out += "HTTP/1.1 200 OK\r\n"
                         "Content-Type: " + m_staticMime.toUtf8() + "\r\n"
                         "Content-Length: " + QByteArray::number(m_staticData.size()) + "\r\n"
                         "Cache-Control: no-cache, no-store\r\n"
                         "Connection: close\r\n"
                         "\r\n" + m_staticData;
                pump(c);
                closeClient(c);
                return;
            }
            c.out += "HTTP/1.1 200 OK\r\n"
                     "Content-Type: " + m_streamMime.toUtf8() + "\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "Cache-Control: no-cache, no-store\r\n"
                     "Connection: close\r\n"
                     "\r\n";
        } else {
            const QByteArray page =
                "<!doctype html><html><meta charset=\"utf-8\"><title>DLNA-Caster</title>"
                "<body><h1>DLNA-Caster</h1>"
                "<p>流地址: <a href=\"/stream\">/stream</a></p></body></html>";
            c.out += "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: " + QByteArray::number(page.size()) +
                     "\r\nConnection: close\r\n\r\n" + page;
            pump(c);
            closeClient(c);
            return;
        }
    }
    pump(c);
}

void MediaServer::onClientDisconnected()
{
    auto *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock)
        return;
    m_clients.remove(sock);
    sock->deleteLater();
}

void MediaServer::onClientBytesWritten(qint64)
{
    auto *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock || !m_clients.contains(sock))
        return;
    pump(m_clients[sock]);
}

void MediaServer::onProcessReadyRead()
{
    if (!m_proc)
        return;
    const QByteArray data = m_proc->readAllStandardOutput();
    if (data.isEmpty())
        return;

    // 分成一个 chunked 帧, 交给所有正在播放的客户端
    const QByteArray frame =
        QByteArray::number(data.size(), 16).toUpper() + "\r\n" + data + "\r\n";
    for (Client &c : m_clients) {
        if (c.streaming) {
            c.out += frame;
            pump(c);
        }
    }
}

void MediaServer::onProcessFinished(int, QProcess::ExitStatus)
{
    for (Client &c : m_clients) {
        if (c.streaming) {
            c.out += "0\r\n\r\n";
            pump(c);
            c.sock->disconnectFromHost();
        }
    }
}

void MediaServer::pump(Client &c)
{
    if (c.out.isEmpty())
        return;
    const qint64 n = c.sock->write(c.out);
    if (n > 0) {
        c.out.remove(0, int(n));
    } else if (n < 0) {
        closeClient(c);
    }
}

void MediaServer::closeClient(Client &c)
{
    // 先从列表移除, 再优雅关闭: 让 socket 把已写入的数据刷完后再断开,
    // 断开后由 onClientDisconnected 负责 deleteLater, 避免数据被丢弃
    m_clients.remove(c.sock);
    c.sock->disconnectFromHost();
}
