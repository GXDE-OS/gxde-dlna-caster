#pragma once
#include <QObject>
#include <QProcess>
#include "renderer.h"
#include "mediaserver.h"
#include "avcontrol.h"

// 投屏控制: 负责 ffmpeg 进程、流服务、SOAP 指令的编排
class CastController : public QObject
{
    Q_OBJECT
public:
    explicit CastController(QObject *parent = nullptr);
    ~CastController() override;

    bool isCasting() const { return m_casting; }
    QString streamUrl() const { return m_streamUrl; }

    void startCasting(const Renderer &target, const CastOptions &opts,
                      bool previewOnly);
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

    QStringList buildFfmpegArgs(const CastOptions &opts, MediaKind kind);
    static MediaKind classifyFile(const QString &path);
    static QString imageMime(const QString &path);
    QString detectMonitorSource();
    static QString findLanIp();
    static QString doubleRate(const QString &rate);

    QProcess *m_proc = nullptr;
    MediaServer m_server;
    AvControl m_av;
    Renderer m_target;
    QString m_streamUrl;
    bool m_casting = false;
};
