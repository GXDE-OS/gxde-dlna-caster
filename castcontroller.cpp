#include "castcontroller.h"
#include <QNetworkInterface>
#include <QNetworkAddressEntry>
#include <QFile>
#include <QFileInfo>

CastController::CastController(QObject *parent)
    : QObject(parent)
    , m_server(this)
    , m_av(this)
{
    connect(&m_av, &AvControl::started, this, [this]() {
        emit logMessage(QStringLiteral("已向设备发送播放指令 (SetAVTransportURI + Play)"));
        emit castStarted();
    });
    connect(&m_av, &AvControl::commandResult, this, [this](bool ok, const QString &error) {
        if (!ok)
            emit castError(QStringLiteral("向设备发送控制指令失败: %1").arg(error));
    });
}

CastController::~CastController()
{
    if (m_proc) {
        m_proc->terminate();
        if (!m_proc->waitForFinished(2000))
            m_proc->kill();
        delete m_proc;
    }
}

void CastController::startCasting(const Renderer &target, const CastOptions &opts,
                                  bool previewOnly)
{
    if (m_casting)
        return;
    m_target = target;
    m_casting = true;

    MediaKind kind = MediaKind::Screen;
    if (!opts.sourceFile.isEmpty())
        kind = classifyFile(opts.sourceFile);

    if (kind == MediaKind::Screen && qgetenv("DISPLAY").isEmpty()) {
        emit castError(QStringLiteral(
            "未检测到 X11 显示环境 (DISPLAY 为空), 无法采集桌面。\n请选择本地媒体文件作为投屏源。"));
        m_casting = false;
        return;
    }

    const QString ip = findLanIp();
    m_streamUrl = QStringLiteral("http://%1:%2/stream").arg(ip).arg(opts.port);
    emit logMessage(QStringLiteral("流地址: %1").arg(m_streamUrl));

    if (!m_server.start(opts.port)) {
        emit castError(QStringLiteral("无法监听端口 %1, 可能被占用").arg(opts.port));
        m_casting = false;
        return;
    }

    // ===== 图片: 直接推送静态文件, 无需 ffmpeg =====
    if (kind == MediaKind::Image) {
        QFile f(opts.sourceFile);
        if (!f.open(QIODevice::ReadOnly)) {
            emit castError(QStringLiteral("无法读取图片文件: %1").arg(opts.sourceFile));
            m_server.stop();
            m_casting = false;
            return;
        }
        const QString mime = imageMime(opts.sourceFile);
        m_server.setStaticFile(f.readAll(), mime);
        emit logMessage(QStringLiteral("图片模式: %1")
                            .arg(QFileInfo(opts.sourceFile).fileName()));

        if (previewOnly || !target.valid()) {
            emit logMessage(QStringLiteral("预览模式: 未向设备发送指令"));
            emit castStarted();
            return;
        }
        emit logMessage(QStringLiteral("投屏目标: %1").arg(target.name));
        m_av.startCasting(target.controlUrl, m_streamUrl, mime);
        return;
    }

    // ===== 视频/桌面/音乐: ffmpeg 编码后流式输出 =====
    const QString mime = (kind == MediaKind::Audio)
                             ? QStringLiteral("audio/mpeg")
                             : QStringLiteral("video/mp2t");
    m_server.setStreamContentType(mime);

    m_proc = new QProcess(this);
    m_proc->setProgram("ffmpeg");
    m_proc->setArguments(buildFfmpegArgs(opts, kind));
    connect(m_proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &CastController::onProcessFinished);
    connect(m_proc, &QProcess::readyReadStandardError, this, [this]() {
        const QByteArray err = m_proc->readAllStandardError();
        if (!err.trimmed().isEmpty())
            emit logMessage(QStringLiteral("[ffmpeg] %1").arg(QString::fromUtf8(err).trimmed()));
    });
    m_server.setStreamProcess(m_proc);

    emit logMessage(kind == MediaKind::Audio
                        ? QStringLiteral("音乐模式: 转码为 MP3 ...")
                        : QStringLiteral("启动 ffmpeg 采集/编码 ..."));
    m_proc->start();
    if (!m_proc->waitForStarted(3000)) {
        emit castError(QStringLiteral("ffmpeg 启动失败, 请确认已安装 ffmpeg (sudo apt install ffmpeg)"));
        m_server.stop();
        m_casting = false;
        return;
    }

    if (previewOnly || !target.valid()) {
        emit logMessage(QStringLiteral("预览模式: 未向设备发送指令, 可用 VLC 打开上面的流地址预览"));
        emit castStarted();
        return;
    }

    emit logMessage(QStringLiteral("投屏目标: %1").arg(target.name));
    m_av.startCasting(target.controlUrl, m_streamUrl, mime);
}

void CastController::stopCasting()
{
    if (!m_casting)
        return;
    m_casting = false;

    if (m_target.valid())
        m_av.stop(m_target.controlUrl);

    if (m_proc) {
        m_proc->terminate();
        if (!m_proc->waitForFinished(2000))
            m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_server.stop();
    m_server.setStreamProcess(nullptr);
    m_server.setStaticFile(QByteArray(), QString());

    emit logMessage(QStringLiteral("已停止投屏"));
    emit castStopped();
}

void CastController::onProcessFinished(int code, QProcess::ExitStatus)
{
    if (!m_casting)
        return;
    m_casting = false;
    if (m_proc) {
        const QByteArray err = m_proc->readAllStandardError();
        if (!err.trimmed().isEmpty())
            emit logMessage(QStringLiteral("[ffmpeg] %1").arg(QString::fromUtf8(err).trimmed()));
    }
    emit logMessage(QStringLiteral("ffmpeg 已退出 (code %1)").arg(code));
    m_server.stop();
    emit castStopped();
}

QStringList CastController::buildFfmpegArgs(const CastOptions &o, MediaKind kind)
{
    QStringList a;
    a << "-hide_banner" << "-loglevel" << "warning" << "-y";

    if (kind == MediaKind::Screen) {
        a << "-f" << "x11grab"
          << "-framerate" << QString::number(o.fps)
          << "-draw_mouse" << "1"
          << "-i" << QString::fromUtf8(qgetenv("DISPLAY"));
        if (o.audio)
            a << "-f" << "pulse" << "-i" << detectMonitorSource();
    } else {
        a << "-re" << "-i" << o.sourceFile;
    }

    if (kind == MediaKind::Audio) {
        // 仅音频: 转码为 MP3
        a << "-vn"
          << "-c:a" << "libmp3lame"
          << "-b:a" << o.audioBitrate
          << "-ar" << "44100" << "-ac" << "2"
          << "-f" << "mp3" << "-";
        return a;
    }

    if (o.scale > 0)
        a << "-vf" << QStringLiteral("scale=-2:%1").arg(o.scale);

    a << "-c:v" << "libx264"
      << "-preset" << "veryfast"
      << "-tune" << "zerolatency"
      << "-pix_fmt" << "yuv420p"
      << "-b:v" << o.bitrate
      << "-maxrate" << o.bitrate
      << "-bufsize" << doubleRate(o.bitrate)
      << "-g" << "30" << "-keyint_min" << "30" << "-sc_threshold" << "0";

    if (o.audio)
        a << "-c:a" << "aac" << "-b:a" << o.audioBitrate
          << "-ar" << "44100" << "-ac" << "2";
    else
        a << "-an";

    a << "-f" << "mpegts" << "-mpegts_flags" << "+resend_headers" << "-";
    return a;
}

CastController::MediaKind CastController::classifyFile(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    static const QStringList images = { "jpg", "jpeg", "png", "bmp", "gif", "webp" };
    static const QStringList audios = { "mp3", "wav", "flac", "aac", "m4a", "ogg", "opus", "wma" };
    if (images.contains(ext))
        return MediaKind::Image;
    if (audios.contains(ext))
        return MediaKind::Audio;
    return MediaKind::Video;
}

QString CastController::imageMime(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "png")
        return QStringLiteral("image/png");
    if (ext == "gif")
        return QStringLiteral("image/gif");
    if (ext == "bmp")
        return QStringLiteral("image/bmp");
    if (ext == "webp")
        return QStringLiteral("image/webp");
    return QStringLiteral("image/jpeg");
}

QString CastController::detectMonitorSource()
{
    QProcess p;
    p.start("pactl", QStringList() << "list" << "sources" << "short");
    if (!p.waitForFinished(3000))
        return QStringLiteral("default");
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList parts = line.split('\t');
        if (parts.size() >= 2 && parts[1].contains(".monitor"))
            return parts[1];
    }
    return QStringLiteral("default");
}

QString CastController::findLanIp()
{
    QString fallback;
    const QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp))
            continue;
        if (iface.flags() & QNetworkInterface::IsLoopBack)
            continue;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            const QString s = addr.toString();
            if (s.startsWith("192.168.") || s.startsWith("10.") ||
                (s.startsWith("172.") && s.mid(4, 2).toInt() >= 16 &&
                 s.mid(4, 2).toInt() <= 31))
                return s;
            if (fallback.isEmpty())
                fallback = s;
        }
    }
    return fallback.isEmpty() ? QStringLiteral("127.0.0.1") : fallback;
}

QString CastController::doubleRate(const QString &rate)
{
    if (rate.size() < 2)
        return rate;
    bool ok = false;
    const int n = rate.left(rate.size() - 1).toInt(&ok);
    return ok ? QString::number(n * 2) + rate.right(1) : rate;
}
