#pragma once
#include <QObject>
#include <QProcess>
#include <memory>
#include "renderer.h"
#include "mediaserver.h"
#include "avcontrol.h"
#include "browserserver.h"
#include "pipewirecapture.h"

class QTemporaryDir;

// 投屏控制: 负责 ffmpeg 进程、流服务、SOAP 指令的编排
class CastController : public QObject
{
    Q_OBJECT
public:
    explicit CastController(QObject *parent = nullptr);
    ~CastController() override;

    bool isCasting() const { return m_casting; }
    QString streamUrl() const { return m_streamUrl; }
    // 浏览器投屏所有网卡可访问的地址 (可能多个, 接收方任选其一打开)
    QStringList browserUrls() const { return m_browserUrls; }
    bool isBrowserMode() const { return m_mode == Mode::Browser; }

    void startCasting(const Renderer &target, const CastOptions &opts,
                      bool previewOnly);
    void startBrowserCasting(const CastOptions &opts);
    void stopCasting();

signals:
    void logMessage(const QString &msg);
    void castStarted();
    void castStopped();
    void castError(const QString &msg);

private slots:
    void onProcessFinished(int code, QProcess::ExitStatus status);

private:
    enum class MediaKind { Screen, Video, Audio, Image };
    enum class Mode { None, Dlna, Browser };

    QStringList buildFfmpegArgs(const CastOptions &opts, MediaKind kind, bool browser);
    static MediaKind classifyFile(const QString &path);
    static QString imageMime(const QString &path);
    static bool hasAudioTrack(const QString &file);
    // 用 ffprobe 获取视频宽高, 失败返回 (0,0)
    static bool getMediaDimensions(const QString &file, int &width, int &height);
    // 检测当前会话是否为 Wayland (此时桌面采集需走 xdg-desktop-portal)
    static bool isWaylandSession();
    // Wayland 桌面采集: 通过 portal + libpipewire 拉取桌面帧交给 ffmpeg (rawvideo FIFO)
    void startScreenCaptureAsync(const CastOptions &opts, bool browser);
    void onCaptureResolution(int width, int height);
    QString detectMonitorSource();
    static QStringList findLanIps();
    static QString findLanIp();
    static QString doubleRate(const QString &rate);

    QProcess *m_proc = nullptr;
    MediaServer m_server;
    BrowserServer m_browserServer;
    AvControl m_av;
    Renderer m_target;
    QString m_streamUrl;
    QStringList m_browserUrls;
    Mode m_mode = Mode::None;
    bool m_casting = false;

    // Wayland 桌面采集 (portal + libpipewire) 状态
    PipeWireCapture m_pwCapture;
    std::unique_ptr<QTemporaryDir> m_captureDir;
    QString m_captureFifo;
    int m_captureFps = 30;
    bool m_captureReady = false;
    Renderer m_pendingTarget;
    CastOptions m_pendingOpts;
    bool m_pendingPreview = false;
    bool m_pendingBrowser = false;
};
