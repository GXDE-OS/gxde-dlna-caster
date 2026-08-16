#include "pipewirecapture.h"

#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusConnection>
#include <QDBusArgument>
#include <QEventLoop>
#include <QDateTime>
#include <QDebug>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <pipewire/pipewire.h>
#include <pipewire/loop.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/param/format.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/debug/types.h>

namespace {
const char kPortalBus[] = "org.freedesktop.portal.Desktop";
const char kPortalPath[] = "/org/freedesktop/portal/desktop";
const char kScreenCastIface[] = "org.freedesktop.portal.ScreenCast";
const char kRequestIface[] = "org.freedesktop.portal.Request";

// libpipewire 采集上下文 (在 run() 线程内使用)
struct PwCtx
{
    PipeWireCapture *self = nullptr;
    QString fifoPath;
    int fps = 30;
    pw_main_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    int fifoFd = -1;
    QByteArray latestFrame;
    bool hasFrame = false;
    qint64 lastSourceFrameMs = 0;
    struct spa_source *timerSource = nullptr;
};

QString randomToken()
{
    return QStringLiteral("gxde%1")
        .arg(QDateTime::currentMSecsSinceEpoch() % 1000000);
}

// ---------- libpipewire 事件回调 ----------
static void onStreamParamChanged(void *data, uint32_t id, const spa_pod *param)
{
    auto *c = static_cast<PwCtx *>(data);
    if (!param || id != SPA_PARAM_Format)
        return;
    spa_video_info info;
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &info.info.raw) < 0)
        return;
    if (info.info.raw.size.width > 0 && info.info.raw.size.height > 0) {
        c->width = int(info.info.raw.size.width);
        c->height = int(info.info.raw.size.height);
        c->stride = c->width * 4;
        emit c->self->resolutionReady(c->width, c->height);

        // P.W. needs to negotiate buffers, otherwise process callback won't be called.
        // Here we ONLY accept the MEMFD buffer type so that we can write the frame data into FIFO directly.
        uint8_t paramsBuffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(paramsBuffer, sizeof(
            paramsBuffer));
        const spa_pod *params[1];
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 1, 32),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(c->stride * c->height),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(c->stride),
            SPA_PARAM_BUFFERS_align, SPA_POD_Int(16),
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int(1u << SPA_DATA_MemFd)));
        pw_stream_update_params(c->stream, params, 1);
    }
}

static void onStreamProcess(void *data)
{
    auto *c = static_cast<PwCtx *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
    if (!b)
        return;
    spa_buffer *buf = b->buffer;
    if (buf->datas && buf->datas[0].chunk) {
        const uint32_t size = buf->datas[0].chunk->size;
        if (size > 0) {
            const uint8_t *frameData = static_cast<const uint8_t *>(buf->datas[0].data);
            QByteArray fallback;
            if (!frameData && buf->datas[0].type == SPA_DATA_MemFd
                && buf->datas[0].fd >= 0 && buf->datas[0].chunk) {
                fallback.resize(int(size));
                const ssize_t n = ::pread(buf->datas[0].fd, fallback.data(), size,
                    buf->datas[0].chunk->offset);
                if (n == ssize_t(size)) {
                    frameData = reinterpret_cast<const uint8_t *>(fallback.constData());
                }
            }
            if (!frameData) {
                pw_stream_queue_buffer(c->stream, b);
                return;
            }

            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (!c->hasFrame || now - c->lastSourceFrameMs >= 1000 / qMax(1, c->fps)) {
                c->latestFrame = QByteArray(reinterpret_cast<const char *>(frameData), int(size));
                c->hasFrame = true;
                c->lastSourceFrameMs = now;
            }
        }
    }
    pw_stream_queue_buffer(c->stream, b);
}

static void onFrameTimer(void *data, uint64_t)
{
    auto *c = static_cast<PwCtx *>(data);
    if (!c->hasFrame || c->latestFrame.isEmpty())
        return;

    if (c->fifoFd < 0) {
        const QByteArray path = c->fifoPath.toUtf8();
        c->fifoFd = ::open(path.constData(), O_WRONLY);
        if (c->fifoFd < 0)
            return;
    }

    const char *p = c->latestFrame.constData();
    qsizetype remaining = c->latestFrame.size();
    while (remaining > 0) {
        const ssize_t n = ::write(c->fifoFd, p, size_t(remaining));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        p += n;
        remaining -= n;
    }
}

} // namespace

PipeWireCapture::PipeWireCapture(QObject *parent)
    : QThread(parent)
{
}

PipeWireCapture::~PipeWireCapture()
{
    stopCapture();
    if (m_running)
        wait(3000);
}

void PipeWireCapture::startCapture(const QString &fifoPath, int targetFps)
{
    if (m_running)
        return;
    m_fifoPath = fifoPath;
    m_fps = qBound(1, targetFps, 120);
    m_stop = false;
    m_running = true;
    start();
}

void PipeWireCapture::stopCapture()
{
    m_stop = true;
    // 解除采集线程可能阻塞在 FIFO 写端 open 的状态:
    // 以只读方式打开再关闭, 使 O_WRONLY open 返回
    if (!m_fifoPath.isEmpty()) {
        const QByteArray path = m_fifoPath.toUtf8();
        const int fd = ::open(path.constData(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0)
            ::close(fd);
    }
    if (m_loop)
        pw_main_loop_quit(static_cast<pw_main_loop *>(m_loop));
}

// 调用 portal 方法并同步等待 Request.Response 回调结果
QVariantMap PipeWireCapture::portalCall(const QString &method, const QVariantList &args)
{
    m_pendingResult.clear();
    QDBusInterface iface(QString::fromLatin1(kPortalBus),
                         QString::fromLatin1(kPortalPath),
                         QString::fromLatin1(kScreenCastIface),
                         QDBusConnection::sessionBus());
    QDBusMessage msg = iface.callWithArgumentList(QDBus::Block, method, args);
    QDBusReply<QDBusObjectPath> reply(msg);
    if (!reply.isValid()) {
        qWarning() << "portal" << method << "failed:" << reply.error().message();
        return m_pendingResult;
    }
    const QString requestPath = reply.value().path();

    QEventLoop loop;
    m_pendingLoop = &loop;
    const bool ok = QDBusConnection::sessionBus().connect(
        QString(), requestPath, QString::fromLatin1(kRequestIface),
        QStringLiteral("Response"), this,
        SLOT(onPortalResponse(uint, QVariantMap)));
    if (!ok) {
        qWarning() << "portal" << method << "signal connect failed";
        m_pendingLoop = nullptr;
        return m_pendingResult;
    }
    loop.exec();
    QDBusConnection::sessionBus().disconnect(
        QString(), requestPath, QString::fromLatin1(kRequestIface),
        QStringLiteral("Response"), this,
        SLOT(onPortalResponse(uint, QVariantMap)));
    m_pendingLoop = nullptr;
    return m_pendingResult;
}

void PipeWireCapture::onPortalResponse(uint, const QVariantMap &results)
{
    m_pendingResult = results;
    if (m_pendingLoop)
        m_pendingLoop->quit();
}

int PipeWireCapture::openPipeWireRemote(const QString &sessionHandle)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPortalBus),
        QString::fromLatin1(kPortalPath),
        QString::fromLatin1(kScreenCastIface),
        QStringLiteral("OpenPipeWireRemote"));
    msg << QVariant::fromValue(QDBusObjectPath(sessionHandle)) << QVariantMap();

    const QDBusMessage reply =
        QDBusConnection::sessionBus().call(msg, QDBus::Block);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qWarning() << "Portal OpenPipeWireRemote failed:"
            << reply.errorMessage();
        return -1;
    }
    if (reply.arguments().isEmpty()) {
        qWarning() << "Portal OpenPipeWireRemote returned no fd";
        return -1;
    }

    const QDBusUnixFileDescriptor descriptor =
        qdbus_cast<QDBusUnixFileDescriptor>(reply.arguments().first());
    if (!descriptor.isValid() || descriptor.fileDescriptor() < 0) {
        qWarning() << "Portal OpenPipeWireRemote returned invalid fd";
        return -1;
    }

    // Duplicate the fd to avoid colsing the portal returned fd when QDBusUnixFileDescriptor is destructed
    return ::dup(descriptor.fileDescriptor());
}

void PipeWireCapture::run()
{
    // ---------- 1. xdg-desktop-portal ScreenCast 授权 ----------
    const QString handleToken = randomToken();
    const QString sessionToken = randomToken();

    QVariantMap sessionOpts;
    sessionOpts.insert(QStringLiteral("handle_token"), handleToken);
    sessionOpts.insert(QStringLiteral("session_handle_token"), sessionToken);
    const QVariantMap sessionResult =
        portalCall(QStringLiteral("CreateSession"), { sessionOpts });
    const QString sessionHandle =
        sessionResult.value(QStringLiteral("session_handle")).toString();
    if (sessionHandle.isEmpty()) {
        emit captureError(tr("无法创建桌面采集会话 (xdg-desktop-portal 不可用?)"));
        m_running = false;
        return;
    }

    QVariantMap selectOpts;
    selectOpts.insert(QStringLiteral("handle_token"), randomToken());
    selectOpts.insert(QStringLiteral("types"), uint(1));      // 1 = Monitor
    selectOpts.insert(QStringLiteral("multiple"), false);
    selectOpts.insert(QStringLiteral("cursor_mode"), uint(1)); // 光标嵌入画面
    portalCall(QStringLiteral("SelectSources"),
               { QVariant::fromValue(QDBusObjectPath(sessionHandle)), selectOpts });

    QVariantMap startOpts;
    startOpts.insert(QStringLiteral("handle_token"), randomToken());
    const QVariantMap startResult = portalCall(
        QStringLiteral("Start"),
        { QVariant::fromValue(QDBusObjectPath(sessionHandle)), QString(), startOpts });

    bool hasTargetNode = false;
    uint32_t targetNodeId = 0;
    const QVariant streamsVar = startResult.value(QStringLiteral("streams"));
    if (streamsVar.canConvert<QDBusArgument>()) {
        const QDBusArgument streams = streamsVar.value<QDBusArgument>();
        streams.beginArray();
        while (!streams.atEnd()) {
            streams.beginStructure();
            uint32_t nodeId = 0;
            QVariantMap props;
            streams >> nodeId;
            streams >> props;
            streams.endStructure();
            if (!hasTargetNode) {
                targetNodeId = nodeId;
                hasTargetNode = true;
            }
        }
        streams.endArray();
    }
    if (!hasTargetNode) {
        qWarning() << "portal Start returned no usable node";
    } else {
        qInfo() << "portal screen cast target node:" << targetNodeId;
    }

    const int pipewireFd = openPipeWireRemote(sessionHandle);
    if (pipewireFd < 0) {
        emit captureError(tr("未从桌面门户获取到视频流"));
        m_running = false;
        return;
    }

    // ---------- 2. libpipewire 拉取桌面帧 ----------
    pw_init(nullptr, nullptr);

    PwCtx ctx;
    ctx.self = this;
    ctx.fifoPath = m_fifoPath;
    ctx.fps = m_fps;
    m_loop = ctx.loop = pw_main_loop_new(nullptr);
    ctx.context = pw_context_new(pw_main_loop_get_loop(ctx.loop), nullptr, 0);
    if (!ctx.context) {
        emit captureError(tr("PipeWire 上下文创建失败"));
        pw_main_loop_destroy(ctx.loop);
        m_loop = nullptr;
        m_running = false;
        return;
    }
    // 连接到 portal 提供的独立 PipeWire 实例
    ctx.core = pw_context_connect_fd(ctx.context, ::dup(pipewireFd), nullptr, 0);
    ::close(pipewireFd);
    if (!ctx.core) {
        emit captureError(tr("PipeWire 连接失败"));
        pw_context_destroy(ctx.context);
        pw_main_loop_destroy(ctx.loop);
        m_loop = nullptr;
        m_running = false;
        return;
    }

    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        nullptr);
    ctx.stream = pw_stream_new(ctx.core, "gxde-caster-screen", props);
    if (!ctx.stream) {
        emit captureError(tr("PipeWire 流创建失败"));
        pw_core_disconnect(ctx.core);
        pw_context_destroy(ctx.context);
        pw_main_loop_destroy(ctx.loop);
        m_loop = nullptr;
        m_running = false;
        return;
    }

    // 请求 BGRA 原始视频
    uint8_t paramsBuffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(paramsBuffer, sizeof(paramsBuffer));
    const spa_pod *params[1];
    params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA)));

    static const pw_stream_events events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .param_changed = onStreamParamChanged,
        .process = onStreamProcess,
    };
    spa_hook streamListener{};
    pw_stream_add_listener(ctx.stream, &streamListener, &events, &ctx);

    const int flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS;
    if (pw_stream_connect(ctx.stream, PW_DIRECTION_INPUT,
                          hasTargetNode ? targetNodeId : PW_ID_ANY,
                          static_cast<pw_stream_flags>(flags), params, 1) < 0) {
        emit captureError(tr("PipeWire 流连接失败"));
        pw_stream_destroy(ctx.stream);
        pw_core_disconnect(ctx.core);
        pw_context_destroy(ctx.context);
        pw_main_loop_destroy(ctx.loop);
        m_loop = nullptr;
        m_running = false;
        return;
    }

    const uint32_t frameIntervalNs = 1000000000u / uint32_t(qMax(1, m_fps));
    struct timespec timerValue{};
    struct timespec timerInterval{};
    timerValue.tv_nsec = frameIntervalNs;
    timerInterval.tv_nsec = frameIntervalNs;
    ctx.timerSource = pw_loop_add_timer(pw_main_loop_get_loop(ctx.loop),
                                        onFrameTimer, &ctx);
    if (ctx.timerSource) {
        pw_loop_update_timer(pw_main_loop_get_loop(ctx.loop), ctx.timerSource,
                             &timerValue, &timerInterval, false);
    } else {
        qWarning() << "failed to create PipeWire frame timer";
    }

    // 主循环 (阻塞, 直到 stopCapture 调用 quit)
    pw_main_loop_run(ctx.loop);

    // ---------- 3. 清理 ----------
    if (ctx.timerSource) {
        pw_loop_destroy_source(pw_main_loop_get_loop(ctx.loop), ctx.timerSource);
        ctx.timerSource = nullptr;
    }
    if (ctx.fifoFd >= 0)
        ::close(ctx.fifoFd);
    if (ctx.stream)
        pw_stream_destroy(ctx.stream);
    if (ctx.core)
        pw_core_disconnect(ctx.core);
    if (ctx.context)
        pw_context_destroy(ctx.context);
    if (ctx.loop)
        pw_main_loop_destroy(ctx.loop);
    m_loop = nullptr;
    m_running = false;
}
