#pragma once
#include <QThread>
#include <QString>
#include <QVariantMap>

class QEventLoop;

// Wayland 桌面采集:
//   通过 xdg-desktop-portal ScreenCast 授权获取桌面流, 用 libpipewire 拉取
//   原始帧 (BGRA), 写入指定 FIFO 文件, 供 ffmpeg -f rawvideo 读取编码。
//   Wayland 会话的桌面采集统一走此流程 (portal 授权 + ffmpeg 编码)。
class PipeWireCapture : public QThread
{
    Q_OBJECT
public:
    explicit PipeWireCapture(QObject *parent = nullptr);
    ~PipeWireCapture() override;

    // fifoPath: 帧输出目标 (调用方先 mkfifo); targetFps: 转发帧率上限
    void startCapture(const QString &fifoPath, int targetFps);
    void stopCapture();
    bool captureRunning() const { return m_running; }

signals:
    // 已协商出桌面分辨率 (在启动 ffmpeg 之后、写帧之前发出)
    void resolutionReady(int width, int height);
    void captureError(const QString &msg);

private slots:
    void onPortalResponse(uint code, const QVariantMap &results);

protected:
    void run() override;

private:
    // 调用 portal 方法并同步等待 Request.Response
    QVariantMap portalCall(const QString &method, const QVariantList &args);

    QString m_fifoPath;
    int m_fps = 30;
    volatile bool m_stop = false;
    volatile bool m_running = false;
    void *m_loop = nullptr;   // pw_main_loop*, 由 run() 设置, stopCapture 只用于唤醒
    QVariantMap m_pendingResult;
    QEventLoop *m_pendingLoop = nullptr;
};
