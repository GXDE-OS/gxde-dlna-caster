#include "browserserver.h"
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>

namespace {
// 播放器页面: 现代深色风格; 图片模式直接显示, 视频/音频走 MSE + WebSocket
// 视频/图片尺寸依照源文件宽高比自适应, 不强制拉伸
const char kPage[] = R"(<!doctype html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GXDE Caster</title>
<script src="/mpegts.js"></script>
<style>
:root { --bg:#11151c; --card:#1a2029; --text:#e8ecf3; --sub:#8b94a5; --accent:#3aa5f8; }
* { margin:0; box-sizing:border-box; }
body { background:var(--bg); color:var(--text); font-family:system-ui,"Noto Sans CJK SC","PingFang SC",sans-serif; min-height:100vh; display:flex; align-items:center; justify-content:center; padding:24px; }
.card { background:var(--card); border:1px solid #262e3a; border-radius:18px; padding:26px; width:fit-content; max-width:min(980px,100%); box-shadow:0 16px 48px rgba(0,0,0,.4); }
h1 { font-size:20px; letter-spacing:.3px; }
.head { display:flex; align-items:center; gap:10px; margin-bottom:6px; }
.badge { background:rgba(58,165,248,.15); color:var(--accent); font-size:11px; padding:2px 8px; border-radius:999px; }
.sub { color:var(--sub); font-size:13px; margin-bottom:16px; }
.media-wrap { display:flex; justify-content:center; align-items:center; }
video,img { display:block; border-radius:10px; background:#000; max-width:100%; max-height:72vh; width:auto; height:auto; object-fit:contain; }
/* 音频播放器也保持合理宽度 */
audio { width:100%; }
#status { color:var(--sub); font-size:13px; margin-top:14px; min-height:1.2em; }
.dot { display:inline-block; width:8px; height:8px; border-radius:50%; background:var(--sub); margin-right:6px; vertical-align:middle; }
.dot.on { background:#2fd07b; }
</style>
</head>
<body>
<div class="card">
  <div class="head"><h1>GXDE Caster</h1><span class="badge">浏览器投屏</span></div>
  <div class="sub" id="sub">正在连接投屏...</div>
  <div id="stage" style="display:none" class="media-wrap">
    <video id="v" autoplay controls playsinline></video>
    <img id="img" style="display:none" alt="">
  </div>
  <div id="status"><span class="dot" id="dot"></span>连接中...</div>
</div>
<script>
const $ = id => document.getElementById(id);
const stage = $('stage'), video = $('v'), img = $('img'),
      status = $('status'), sub = $('sub'), dot = $('dot');
const ok = () => { dot.className = 'dot on'; };

// 根据宽高比设置元素尺寸, 同时受 max-width/max-height 约束
function applyAspectRatio(el, w, h) {
  if (!w || !h) return;
  el.style.aspectRatio = w + '/' + h;
}

fetch('/info').then(r => r.json()).then(info => {
  stage.style.display = 'flex';
  if (info.type === 'image') {
    sub.textContent = '图片投屏';
    video.style.display = 'none';       // 隐藏视频窗口
    img.style.display = 'block';
    if (info.width && info.height) applyAspectRatio(img, info.width, info.height);
    img.onload = () => {
      // 图片加载完成后用实际尺寸确认比例
      applyAspectRatio(img, img.naturalWidth, img.naturalHeight);
    };
    img.src = info.path || '/image';
    status.textContent = '正在显示图片';
    ok();
    return;
  }
  sub.textContent = info.type === 'audio' ? '音乐投屏' : '视频投屏';
  img.style.display = 'none';           // 隐藏图片元素

  // 提前根据后端给出的宽高设置比例, 避免布局跳动
  if (info.width && info.height) {
    applyAspectRatio(video, info.width, info.height);
  }

  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  const wsUrl = proto + location.hostname + ':' + info.ws_port + '/ws';

  // ===== 音频: 原生 MSE 播放 fMP4 (Firefox/Chrome 均兼容) =====
  if (info.type === 'audio') {
    video.style.display = 'none';
    if (!window.MediaSource) { status.textContent = '当前浏览器不支持 MediaSource'; return; }
    const ms = new MediaSource();
    video.src = URL.createObjectURL(ms);
    let sb = null;
    const queue = [];
    let isAppending = false;
    const okMime = m => { try { return MediaSource.isTypeSupported(m); } catch (e) { return false; } };

    function appendNext() {
      if (!sb || sb.updating || queue.length === 0) {
        isAppending = false;
        return;
      }
      isAppending = true;
      const data = queue.shift();
      try {
        sb.appendBuffer(data);
      } catch (e) {
        isAppending = false;
        if (e.name === 'QuotaExceededError') {
          // 移除已播放超过 5 秒的缓冲, 保留最近 5 秒
          try {
            const removeEnd = Math.max(0, video.currentTime - 5);
            if (removeEnd > 0 && sb.buffered.length > 0) {
              const bufStart = sb.buffered.start(0);
              if (removeEnd > bufStart) {
                sb.remove(bufStart, removeEnd);
                // remove 是异步的, updateend 后会继续 appendNext
                return;
              }
            }
          } catch (rmErr) { console.warn('remove error:', rmErr); }
          // 清理失败, 延迟重试
          setTimeout(() => {
            queue.unshift(data);
            appendNext();
          }, 200);
        } else {
          console.warn('append error:', e, e.name);
          // 出错后继续处理队列中下一块
          setTimeout(appendNext, 50);
        }
      }
    }

    ms.addEventListener('sourceopen', () => {
      let chosen = info.mime;
      if (!okMime(chosen)) {
        const plain = info.mime.replace(/"/g, '');
        if (okMime(plain)) chosen = plain;
        else if (okMime('audio/mp4')) chosen = 'audio/mp4';
      }
      try {
        sb = ms.addSourceBuffer(chosen);
        sb.mode = 'segments';
        sb.addEventListener('updateend', appendNext);
        sb.addEventListener('error', (e) => {
          console.error('SourceBuffer error:', e);
          isAppending = false;
        });
        status.textContent = '连接成功, 正在缓冲...';
        ok();
        appendNext();
      } catch (e) {
        status.textContent = '不支持的编码: ' + chosen + ' (' + e.message + ')';
        console.error('addSourceBuffer error:', e);
      }
    });

    ms.addEventListener('sourceended', () => {
      isAppending = false;
    });

    const ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';
    ws.onmessage = ev => {
      if (ms.readyState !== 'open' && ms.readyState !== 'closed') return;
      queue.push(ev.data);
      if (sb && !sb.updating && !isAppending) {
        appendNext();
      }
    };
    ws.onopen = () => { status.textContent = '已连接, 正在接收数据...'; ok(); };
    ws.onclose = () => { status.textContent = '连接已断开 (投屏可能已结束)'; };
    ws.onerror = () => { status.textContent = '连接失败, 请检查 ' + wsUrl + ' 是否可达'; };
    return;
  }

  // ===== 视频/桌面: 使用 mpegts.js 播放 MPEG-TS (Firefox/Chrome 均兼容) =====
  if (!window.mpegts) { status.textContent = '播放器组件未加载'; return; }
  status.textContent = '连接中, 正在缓冲...';
  ok();
  const player = mpegts.createPlayer({
    type: 'mse',            // MSE 模式, 兼容性最好
    isLive: true,
    url: wsUrl,
  }, {
    liveBufferLatencyChasing: true,
    liveBufferLatencyMaxLatency: 3,   // 直播低延迟
    enableStashBuffer: false,
    lazyLoad: false,
    autoCleanupSourceBuffer: true,
    reuseSourceBuffer: true,
  });
  player.attachMediaElement(video);
  player.on(mpegts.ErrorTypes.NETWORK_ERROR, () => {
    status.textContent = '连接中断, 尝试重连...';
    if (!player.isPlaying())
      player.load();
  });
  player.on(mpegts.ErrorTypes.MEDIA_ERROR, (type, detail) => {
    console.error('media error:', detail);
    if (!player.isPlaying())
      player.load();
  });
  player.load();
  video.addEventListener('playing', () => { status.textContent = '正在播放'; });
  video.addEventListener('loadedmetadata', () => {
    // 元数据加载完成后用实际视频分辨率更新比例 (可能因转码缩放与源文件不同)
    if (video.videoWidth && video.videoHeight) {
      applyAspectRatio(video, video.videoWidth, video.videoHeight);
    }
  });
  player.play().catch(e => console.warn('play:', e));
}).catch(err => {
  status.textContent = '无法获取流信息: ' + err.message;
  console.error('fetch info error:', err);
});
</script>
</body>
</html>
)";
} // namespace

BrowserServer::BrowserServer(QObject *parent)
    : QObject(parent)
    , m_ws(QStringLiteral("gxde-dlna-caster"), QWebSocketServer::NonSecureMode, this)
{
    connect(&m_http, &QTcpServer::newConnection, this, &BrowserServer::onHttpReady);
    connect(&m_ws, &QWebSocketServer::newConnection, this, &BrowserServer::onNewWsConnection);
}

bool BrowserServer::start(quint16 port)
{
    // 重新开始前清空上一次的 init 缓存
    m_http.close();
    m_ws.close();
    m_initReady = false;
    m_initCache.clear();
    m_parseBuf.clear();

    m_wsPort = (port < 65535) ? quint16(port + 1) : quint16(65534);
    if (!m_http.listen(QHostAddress::Any, port))
        return false;
    if (!m_ws.listen(QHostAddress::Any, m_wsPort)) {
        m_http.close();
        return false;
    }
    return true;
}

void BrowserServer::setStreamProcess(QProcess *proc)
{
    if (m_proc)
        disconnect(m_proc, nullptr, this, nullptr);
    m_proc = proc;
    if (!proc)
        return;
    connect(proc, &QProcess::readyReadStandardOutput,
            this, &BrowserServer::onProcessReadyRead);
}

void BrowserServer::setMediaInfo(const QString &type, const QString &mime, int width, int height)
{
    m_mediaType = type;
    m_mediaMime = mime;
    m_mediaWidth = width;
    m_mediaHeight = height;
}

void BrowserServer::setStaticImage(const QByteArray &data, const QString &mime, int width, int height)
{
    m_staticImage = data;
    m_staticMime = mime;
    m_staticWidth = width;
    m_staticHeight = height;
}

void BrowserServer::stop()
{
    for (QWebSocket *sock : qAsConst(m_clients))
        sock->close();
    qDeleteAll(m_clients);
    m_clients.clear();
    m_http.close();
    m_ws.close();
    // 重置状态, 避免下次投屏复用上一次的尺寸
    m_mediaWidth = m_mediaHeight = 0;
    m_staticWidth = m_staticHeight = 0;
}

void BrowserServer::onHttpReady()
{
    while (m_http.hasPendingConnections()) {
        QTcpSocket *sock = m_http.nextPendingConnection();
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
            QByteArray &in = m_httpBuf[sock];
            in += sock->readAll();

            const int end = in.indexOf("\r\n\r\n");
            if (end < 0)
                return;  // 请求头未收全, 等待更多数据
            const QByteArray head = in.left(end);
            const QByteArray first = head.split('\n').value(0);
            m_httpBuf.remove(sock);

            if (first.contains("/info")) {
                QJsonObject info;
                if (m_mediaType == "image") {
                    info = {
                        { "type", QStringLiteral("image") },
                        { "mime", m_staticMime },
                        { "path", QStringLiteral("/image") },
                        { "width", m_staticWidth },
                        { "height", m_staticHeight },
                    };
                } else {
                    info = {
                        { "type", m_mediaType },
                        { "mime", m_mediaMime },
                        { "ws_port", int(m_wsPort) },
                        { "width", m_mediaWidth },
                        { "height", m_mediaHeight },
                    };
                }
                serveText(sock, QJsonDocument(info).toJson(QJsonDocument::Compact),
                          "application/json; charset=utf-8");
                return;
            }
            if (first.contains("/image") && !m_staticImage.isEmpty()) {
                // 一次性写入静态图片; 不立即删除 socket, 等待数据刷完后再断开
                sock->write("HTTP/1.1 200 OK\r\n"
                            "Content-Type: " + m_staticMime.toUtf8() + "\r\n"
                            "Content-Length: " + QByteArray::number(m_staticImage.size()) + "\r\n"
                            "Cache-Control: no-cache\r\n"
                            "Connection: close\r\n\r\n" + m_staticImage);
                sock->disconnectFromHost();
                return;
            }
            if (first.contains("/mpegts.js")) {
                // 本地 mpegts.js 播放器 (打包在 Qt 资源中, 离线可用)
                QFile f(QStringLiteral(":/mpegts/mpegts.js"));
                if (f.open(QIODevice::ReadOnly)) {
                    const QByteArray js = f.readAll();
                    serveText(sock, js, "application/javascript; charset=utf-8");
                    return;
                }
                serveText(sock, "/* mpegts.js not found */", "application/javascript");
                return;
            }
            serveText(sock, QByteArray(kPage), "text/html; charset=utf-8");
        });
        connect(sock, &QTcpSocket::disconnected, this, [this, sock]() {
            m_httpBuf.remove(sock);
            sock->deleteLater();
        });
    }
}

void BrowserServer::onNewWsConnection()
{
    const bool needsInit = m_mediaMime.contains("mp4");
    while (m_ws.hasPendingConnections()) {
        QWebSocket *sock = m_ws.nextPendingConnection();
        m_clients.append(sock);
        if (needsInit) {
            // fMP4 (音频): 新客户端需要 init segment, 已就绪则立即补发, 否则等就绪后补发
            if (m_initReady && !m_initCache.isEmpty())
                sock->sendBinaryMessage(m_initCache);
            else
                m_pendingInit.insert(sock);
        }
        // TS (视频/桌面): 流式数据直接广播即可, 无需 init
        connect(sock, &QWebSocket::disconnected, this, [this, sock]() {
            m_clients.removeAll(sock);
            m_pendingInit.remove(sock);
            sock->deleteLater();
        });
    }
}

void BrowserServer::broadcast(const QByteArray &data)
{
    for (QWebSocket *sock : qAsConst(m_clients)) {
        if (sock->isValid())
            sock->sendBinaryMessage(data);
    }
}

void BrowserServer::onProcessReadyRead()
{
    if (!m_proc)
        return;
    const QByteArray data = m_proc->readAllStandardOutput();
    if (data.isEmpty())
        return;

    // MPEG-TS (视频/桌面): 流式格式, 无需 init segment, 直接转发
    if (!m_mediaMime.contains("mp4")) {
        broadcast(data);
        return;
    }

    // fMP4 (音频): 需要先解析并缓存 init segment, 供新连接的客户端补发
    if (!m_initReady) {
        // 先收集数据, 直到解析出完整 init segment (ftyp + moov + 第一个 moof)
        m_parseBuf += data;
        const int initEnd = findInitEnd(m_parseBuf);
        if (initEnd > 0) {
            m_initCache = m_parseBuf.left(initEnd);
            m_initReady = true;
            const QByteArray rest = m_parseBuf.mid(initEnd);
            m_parseBuf.clear();
            // 给 init 就绪前已连接的客户端补发 init, 否则它们无法解码
            for (QWebSocket *sock : qAsConst(m_pendingInit)) {
                if (sock->isValid())
                    sock->sendBinaryMessage(m_initCache);
            }
            m_pendingInit.clear();
            if (!rest.isEmpty())
                broadcast(rest);
        }
        return;  // init 未就绪前不广播
    }
    broadcast(data);
}

// 简易 MP4 box 解析: 返回 moof box 的起始位置 (即 init segment 末尾, init = ftyp + moov);
// 数据不足(还没看到 moof 或 moov 没收全)时返回 0
int BrowserServer::findInitEnd(const QByteArray &buf)
{
    const auto u32 = [&buf](int pos) -> quint32 {
        return (quint32(quint8(buf[pos])) << 24) | (quint32(quint8(buf[pos + 1])) << 16)
               | (quint32(quint8(buf[pos + 2])) << 8) | quint32(quint8(buf[pos + 3]));
    };
    const auto u64 = [&buf, &u32](int pos) -> quint64 {
        return (quint64(u32(pos)) << 32) | quint64(u32(pos + 4));
    };

    int pos = 0;
    bool sawMoov = false;
    while (pos + 8 <= buf.size()) {
        quint32 size = u32(pos);
        const QByteArray type = buf.mid(pos + 4, 4);
        qint64 boxEnd;
        if (size == 1) {                       // 64 位大 box
            if (pos + 16 > buf.size())
                return 0;
            boxEnd = qint64(pos) + qint64(u64(pos + 8));
        } else if (size == 0) {                // 到文件末尾 (这种情况下如果没看到 moof, 说明数据不足)
            return 0;
        } else {
            if (size < 8)                      // 无效 size
                return 0;
            boxEnd = qint64(pos) + size;
        }
        if (boxEnd > buf.size() || boxEnd <= pos)
            return 0;                          // box 数据未收全
        if (type == "moov")
            sawMoov = true;
        if (type == "moof") {                  // 遇到第一个 moof: init segment 在 moof 之前结束
            // 必须已经收到 moov, 否则 init segment 不完整
            return sawMoov ? pos : 0;
        }
        pos = int(boxEnd);
    }
    return 0;
}

void BrowserServer::serveText(QTcpSocket *sock, const QByteArray &body,
                              const QByteArray &contentType)
{
    // 写入后不立即删除, 由 disconnected 信号负责清理, 保证数据刷完
    sock->write("HTTP/1.1 200 OK\r\n"
                "Content-Type: " + contentType + "\r\n"
                "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n\r\n" + body);
    sock->disconnectFromHost();
}
