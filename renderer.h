#pragma once
#include <QString>
#include <QMetaType>

#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

// 设备探测/控制请求使用的 User-Agent: "GXDE Caster/<版本> <登录名>"
inline QString gxdeUserAgent()
{
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty())
        user = qEnvironmentVariable("LOGNAME");
    if (user.isEmpty())
        user = QStringLiteral("user");
    return QStringLiteral("GXDE Caster/%1 %2").arg(QStringLiteral(APP_VERSION), user);
}

// 一台 DLNA 播放设备 (电视 / 盒子)
struct Renderer
{
    QString name;
    QString location;    // 设备描述 XML 地址
    QString controlUrl;  // AVTransport 服务控制端点
    QString eventUrl;
    QString udn;

    bool valid() const { return !controlUrl.isEmpty(); }
};

// 投屏参数
struct CastOptions
{
    QString sourceFile;   // 空 = 采集桌面屏幕
    int fps = 30;
    int scale = 1080;     // 画面高度上限, 0 = 不缩放
    QString bitrate = "4M";
    QString audioBitrate = "128k";
    bool audio = true;
    quint16 port = 8090;
};

Q_DECLARE_METATYPE(Renderer)
