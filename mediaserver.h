#pragma once
#include <QTcpServer>
#include <QHash>
#include <QProcess>

// HTTP 服务器: 把 ffmpeg 的 stdout 以流式 (chunked) 提供给设备,
// 或直接推送静态文件 (如图片, Content-Length 方式)
class MediaServer : public QTcpServer
{
    Q_OBJECT
public:
    explicit MediaServer(QObject *parent = nullptr);

    bool start(quint16 port);
    void setStreamProcess(QProcess *proc);
    void setStaticFile(const QByteArray &data, const QString &mime);
    void setStreamContentType(const QString &mime);
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
    QByteArray m_staticData;     // 静态文件内容 (图片模式)
    QString m_staticMime;
    QString m_streamMime = QStringLiteral("video/mp2t");  // 流式内容类型
    bool m_staticMode = false;
};
