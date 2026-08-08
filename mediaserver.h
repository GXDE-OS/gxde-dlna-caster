#pragma once
#include <QTcpServer>
#include <QHash>
#include <QProcess>

// HTTP 流媒体服务器: 把 ffmpeg 的 stdout 以 MPEG-TS (chunked) 流式提供给设备
class MediaServer : public QTcpServer
{
    Q_OBJECT
public:
    explicit MediaServer(QObject *parent = nullptr);

    bool start(quint16 port);
    void setStreamProcess(QProcess *proc);
    void stop();

private slots:
    void onNewConnection();
    void onClientReady();
    void onClientDisconnected();
    void onClientBytesWritten(qint64 bytes);
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

private:
    struct Client {
        QTcpSocket *sock = nullptr;
        QByteArray in;
        QByteArray out;      // 待写出的数据 (含 HTTP 头 + chunked 帧)
        bool headersSent = false;
        bool streaming = false;
    };

    void pump(Client &c);
    void closeClient(Client &c);

    QHash<QTcpSocket *, Client> m_clients;
    QProcess *m_proc = nullptr;
};
