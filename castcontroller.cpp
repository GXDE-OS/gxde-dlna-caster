#include "castcontroller.h"
#include <QNetworkInterface>
#include <QNetworkAddressEntry>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QGuiApplication>
#include <QScreen>

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
    m_mode = Mode::Dlna;

    MediaKind kind = MediaKind::Screen;
    if (!opts.sourceFile.isEmpty())
        kind = classifyFile(opts.sourceFile);

    if (kind == MediaKind::Screen) {
        if (isWaylandSession()) {
            if (!ffmpegSupportsPipewire()) {
                emit castError(QStringLiteral(
                    "检测到 Wayland 会话, 桌面采集需要 ffmpeg 支持 PipeWire。\n"
                    "当前 ffmpeg 未编译 PipeWire 输入设备 (需要 ffmpeg >= 5.1 且 --enable-libpipewire)。\n"
                    "请升级 ffmpeg 或选择本地媒体文件作为投屏源。"));
                m_casting = false;
                return;
            }
        } else if (qgetenv("DISPLAY").isEmpty()) {
            emit castError(QStringLiteral(
                "未检测到 X11 显示环境 (DISPLAY 为空), 无法采集桌面。\n请选择本地媒体文件作为投屏源。"));
            m_casting = false;
            return;
        }
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
    m_proc->setArguments(buildFfmpegArgs(opts, kind, false));
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

void CastController::startBrowserCasting(const CastOptions &opts)
{
    if (m_casting)
        return;
    m_target = Renderer();
    m_casting = true;
    m_mode = Mode::Browser;

    MediaKind kind = MediaKind::Screen;
    if (!opts.sourceFile.isEmpty())
        kind = classifyFile(opts.sourceFile);

    if (kind == MediaKind::Screen) {
        if (isWaylandSession()) {
            if (!ffmpegSupportsPipewire()) {
                emit castError(QStringLiteral(
                    "检测到 Wayland 会话, 桌面采集需要 ffmpeg 支持 PipeWire。\n"
                    "当前 ffmpeg 未编译 PipeWire 输入设备 (需要 ffmpeg >= 5.1 且 --enable-libpipewire)。\n"
                    "请升级 ffmpeg 或选择本地媒体文件作为投屏源。"));
                m_casting = false;
                m_mode = Mode::None;
                return;
            }
        } else if (qgetenv("DISPLAY").isEmpty()) {
            emit castError(QStringLiteral(
                "未检测到 X11 显示环境 (DISPLAY 为空), 无法采集桌面。\n请选择本地媒体文件作为投屏源。"));
            m_casting = false;
            m_mode = Mode::None;
            return;
        }
    }

    // 浏览器投屏监听所有网卡, 生成每个网卡对应的访问地址 (接收方任选其一)
    m_browserUrls.clear();
    const QStringList ips = findLanIps();
    for (const QString &ip : ips)
        m_browserUrls << QStringLiteral("http://%1:%2/").arg(ip).arg(opts.port);
    m_streamUrl = m_browserUrls.value(0);
    emit logMessage(QStringLiteral("浏览器投屏地址: %1").arg(m_browserUrls.join(QStringLiteral(", "))));

    if (!m_browserServer.start(opts.port)) {
        emit castError(QStringLiteral("无法监听端口 %1, 可能被占用").arg(opts.port));
        m_casting = false;
        m_mode = Mode::None;
        return;
    }

    // 图片: 浏览器页面直接显示
    if (kind == MediaKind::Image) {
        QFile f(opts.sourceFile);
        if (!f.open(QIODevice::ReadOnly)) {
            emit castError(QStringLiteral("无法读取图片文件: %1").arg(opts.sourceFile));
            m_browserServer.stop();
            m_casting = false;
            m_mode = Mode::None;
            return;
        }
        const QByteArray imgData = f.readAll();
        const QString mime = imageMime(opts.sourceFile);
        // 获取图片原始尺寸
        int imgW = 0, imgH = 0;
        QImageReader reader(opts.sourceFile);
        const QSize imgSize = reader.size();
        if (imgSize.isValid()) {
            imgW = imgSize.width();
            imgH = imgSize.height();
        }
        m_browserServer.setStaticImage(imgData, mime, imgW, imgH);
        m_browserServer.setMediaInfo(QStringLiteral("image"), mime, imgW, imgH);
        emit logMessage(QStringLiteral("图片模式: %1 (%2x%3)")
                            .arg(QFileInfo(opts.sourceFile).fileName())
                            .arg(imgW).arg(imgH));
        emit castStarted();
        return;
    }

    // 视频/音乐/桌面: ffmpeg 编码为 fMP4, 经 WebSocket 推给浏览器
    CastOptions eff = opts;
    if (kind == MediaKind::Video && eff.audio && !hasAudioTrack(opts.sourceFile))
        eff.audio = false;

    // 计算输出分辨率
    int outW = 0, outH = 0;
    if (kind == MediaKind::Screen) {
        // 桌面采集: 使用主屏幕分辨率
        if (QScreen *scr = QGuiApplication::primaryScreen()) {
            const QSize sz = scr->size();
            outW = sz.width();
            outH = sz.height();
        }
    } else if (kind == MediaKind::Video) {
        int srcW = 0, srcH = 0;
        if (getMediaDimensions(opts.sourceFile, srcW, srcH)) {
            outW = srcW;
            outH = srcH;
        }
    }
    // 如果设置了 scale (高度上限), 按比例缩放
    if (outW > 0 && outH > 0 && eff.scale > 0 && outH > eff.scale) {
        const double ratio = double(eff.scale) / double(outH);
        outH = eff.scale;
        outW = int(outW * ratio);
        // 确保宽度为偶数 (yuv420p 要求)
        if (outW % 2 != 0) outW += 1;
    }

    QString type = QStringLiteral("video");
    QString mime;
    if (kind == MediaKind::Audio) {
        type = QStringLiteral("audio");
        mime = QStringLiteral("audio/mp4; codecs=\"mp4a.40.2\"");
    } else {
        // MPEG-TS 流, 由前端 mpegts.js 播放 (不再依赖 MSE fMP4)
        mime = QStringLiteral("video/mp2t");
    }
    m_browserServer.setMediaInfo(type, mime, outW, outH);

    m_proc = new QProcess(this);
    m_proc->setProgram("ffmpeg");
    m_proc->setArguments(buildFfmpegArgs(eff, kind, true));
    connect(m_proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &CastController::onProcessFinished);
    connect(m_proc, &QProcess::readyReadStandardError, this, [this]() {
        const QByteArray err = m_proc->readAllStandardError();
        if (!err.trimmed().isEmpty())
            emit logMessage(QStringLiteral("[ffmpeg] %1").arg(QString::fromUtf8(err).trimmed()));
    });
    m_browserServer.setStreamProcess(m_proc);

    emit logMessage(QStringLiteral("启动 ffmpeg 编码 (MPEG-TS) ..."));
    m_proc->start();
    if (!m_proc->waitForStarted(3000)) {
        emit castError(QStringLiteral("ffmpeg 启动失败, 请确认已安装 ffmpeg (sudo apt install ffmpeg)"));
        m_browserServer.stop();
        m_casting = false;
        m_mode = Mode::None;
        return;
    }

    emit logMessage(QStringLiteral("浏览器投屏已开始: 在接收设备浏览器中打开 %1").arg(m_streamUrl));
    emit castStarted();
}

void CastController::stopCasting()
{
    if (!m_casting)
        return;
    m_casting = false;

    if (m_mode == Mode::Dlna && m_target.valid())
        m_av.stop(m_target.controlUrl);

    if (m_proc) {
        m_proc->terminate();
        if (!m_proc->waitForFinished(2000))
            m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_server.stop();
    m_browserServer.stop();
    m_server.setStreamProcess(nullptr);
    m_server.setStaticFile(QByteArray(), QString());
    m_browserServer.setStreamProcess(nullptr);
    m_browserUrls.clear();
    m_mode = Mode::None;

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
    // ffmpeg 结束(自然播完/异常)时同样释放所有服务与端口, 避免残留占用
    m_server.stop();
    m_browserServer.stop();
    m_server.setStreamProcess(nullptr);
    m_browserServer.setStreamProcess(nullptr);
    m_browserUrls.clear();
    m_mode = Mode::None;
    emit castStopped();
}

QStringList CastController::buildFfmpegArgs(const CastOptions &o, MediaKind kind,
                                            bool browser)
{
    QStringList a;
    a << "-hide_banner" << "-loglevel" << "warning" << "-y";

    if (kind == MediaKind::Screen) {
        if (isWaylandSession()) {
            // Wayland: 通过 PipeWire 采集桌面 (ffmpeg >= 5.1, 需 --enable-libpipewire)
            a << "-f" << "pipewire" << "-i" << "0";
        } else {
            a << "-f" << "x11grab"
              << "-framerate" << QString::number(o.fps)
              << "-draw_mouse" << "1"
              << "-i" << QString::fromUtf8(qgetenv("DISPLAY"));
        }
        // 音频: PipeWire 提供 pulse 兼容层, 继续使用 pulse 采集系统声音
        if (o.audio)
            a << "-f" << "pulse" << "-i" << detectMonitorSource();
    } else {
        a << "-re" << "-i" << o.sourceFile;
    }

    if (kind == MediaKind::Audio) {
        // 仅音频
        a << "-vn"
          << "-c:a" << (browser ? "aac" : "libmp3lame")
          << "-b:a" << o.audioBitrate
          << "-ar" << "44100" << "-ac" << "2";
        if (browser)
            a << "-f" << "mp4" << "-movflags"
              << "frag_keyframe+empty_moov+default_base_moof" << "-";
        else
            a << "-f" << "mp3" << "-";
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

    // 视频/桌面: 统一输出 MPEG-TS (DLNA 设备与浏览器 mpegts.js 均使用)
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

bool CastController::hasAudioTrack(const QString &file)
{
    QProcess p;
    p.start("ffprobe", QStringList()
            << "-v" << "error"
            << "-select_streams" << "a"
            << "-show_entries" << "stream=index"
            << "-of" << "csv=p=0"
            << file);
    if (!p.waitForFinished(4000))
        return false;
    return !p.readAllStandardOutput().trimmed().isEmpty();
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

QStringList CastController::findLanIps()
{
    QStringList privateIps;
    QStringList otherIps;
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
                privateIps << s;
            else if (!otherIps.contains(s))
                otherIps << s;
        }
    }
    // 私有网段优先, 其余网卡 IP 也一并列出 (含 VPN / 虚拟网卡)
    QStringList result = privateIps;
    for (const QString &ip : qAsConst(otherIps)) {
        if (!result.contains(ip))
            result << ip;
    }
    result.removeDuplicates();
    return result;
}

QString CastController::findLanIp()
{
    return findLanIps().value(0, QStringLiteral("127.0.0.1"));
}

QString CastController::doubleRate(const QString &rate)
{
    if (rate.size() < 2)
        return rate;
    bool ok = false;
    const int n = rate.left(rate.size() - 1).toInt(&ok);
    return ok ? QString::number(n * 2) + rate.right(1) : rate;
}

bool CastController::getMediaDimensions(const QString &file, int &width, int &height)
{
    width = 0;
    height = 0;
    QProcess p;
    p.start("ffprobe", QStringList()
            << "-v" << "error"
            << "-select_streams" << "v:0"
            << "-show_entries" << "stream=width,height"
            << "-of" << "csv=p=0:s=x"
            << file);
    if (!p.waitForFinished(4000))
        return false;
    const QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    // 格式: "WxH"
    const int x = out.indexOf('x');
    if (x <= 0)
        return false;
    bool wok = false, hok = false;
    const int w = out.left(x).toInt(&wok);
    const int h = out.mid(x + 1).toInt(&hok);
    if (!wok || !hok || w <= 0 || h <= 0)
        return false;
    width = w;
    height = h;
    return true;
}

bool CastController::isWaylandSession()
{
    const QByteArray xdg = qgetenv("XDG_SESSION_TYPE").toLower();
    if (xdg == "wayland")
        return true;
    // 部分环境未设置 XDG_SESSION_TYPE, 通过 QT_QPA_PLATFORM / WAYLAND_DISPLAY 判断
    const QByteArray qpa = qgetenv("QT_QPA_PLATFORM").toLower();
    if (qpa.contains("wayland"))
        return true;
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && xdg != "x11")
        return true;
    return false;
}

bool CastController::ffmpegSupportsPipewire()
{
    QProcess p;
    p.start("ffmpeg", QStringList() << "-hide_banner" << "-devices");
    if (!p.waitForFinished(3000))
        return false;
    const QString out = QString::fromUtf8(p.readAllStandardOutput())
                        + QString::fromUtf8(p.readAllStandardError());
    return out.contains(QStringLiteral("pipewire"));
}
