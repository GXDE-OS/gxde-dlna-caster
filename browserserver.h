#pragma once
#include <QTcpServer>
#include <QList>
#include <QSet>
#include <QProcess>
#include <QWebSocketServer>
#include <QWebSocket>

// 浏览器投屏服务端 (Qt6WebSockets):
//   HTTP 端口:   /           -> 播放器页面
//                /info       -> JSON: {type, mime, ws_port}
//                /image      -> 静态图片 (图片模式)
//                /mpegts.js  -> 本地 mpegts.js 播放器 (打包在 Qt 资源中)
//   WS 端口(+1): /ws         -> 二进制帧中继 ffmpeg 数据
//  视频/桌面: MPEG-TS 流 + 前端 mpegts.js 播放 (Firefox/Chrome 均兼容)
//  音频: fMP4 流 + 原生 MediaSource (音频兼容性良好)
class BrowserServer : public QObject
{
    Q_OBJECT
public:
    explicit BrowserServer(QObject *parent = nullptr);

    bool start(quint16 port);        // 同时启动 HTTP 与 WebSocket 监听
    quint16 wsPort() const { return m_wsPort; }
    void setStreamProcess(QProcess *proc);
    void setMediaInfo(const QString &type, const QString &mime, int width = 0, int height = 0);
    void setStaticImage(const QByteArray &data, const QString &mime, int width = 0, int height = 0);
    void stop();

private slots:
    void onHttpReady();
    void onNewWsConnection();
    void onProcessReadyRead();

private:
    void serveText(QTcpSocket *sock, const QByteArray &body, const QByteArray &contentType);
    void broadcast(const QByteArray &data);
    // 从 fMP4 数据流中解析出 init segment 结束位置
    // init segment 包含 ftyp + moov (到第一个 moof box 开始之前)
    // 返回 0 表示数据不足
    static int findInitEnd(const QByteArray &buf);

    QTcpServer m_http;
    QHash<QTcpSocket *, QByteArray> m_httpBuf;
    QWebSocketServer m_ws;
    QList<QWebSocket *> m_clients;
    QProcess *m_proc = nullptr;
    QString m_mediaType = QStringLiteral("video");
    QString m_mediaMime = QStringLiteral("video/mp4; codecs=\"avc1.42E01E,mp4a.40.2\"");
    int m_mediaWidth = 0;
    int m_mediaHeight = 0;
    QByteArray m_staticImage;
    QString m_staticMime;
    int m_staticWidth = 0;
    int m_staticHeight = 0;
    quint16 m_wsPort = 0;

    // 缓存 fMP4 init segment: 新 WS 客户端连接时先补发, 否则 MSE 无法解码
    QByteArray m_initCache;
    QByteArray m_parseBuf;
    bool m_initReady = false;
    QSet<QWebSocket *> m_pendingInit;   // init 就绪前已连接的客户端, 就绪后补发
};
