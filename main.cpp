#include <DApplication>
#include <DWidgetUtil>
#include <QIcon>
#include "mainwindow.h"

DWIDGET_USE_NAMESPACE

int main(int argc, char *argv[])
{
    DApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("gxde"));
    app.setApplicationName(QStringLiteral("gxde-dlna-caster"));
    app.setApplicationDisplayName(QStringLiteral("GXDE DLNA Caster"));
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setApplicationDescription(QStringLiteral(
        "基于 Qt6 + DTK2 的 DLNA 投屏工具，通过 DLNA/UPnP 协议将桌面屏幕、"
        "声音或本地媒体文件投送到电视。\n\n"
        "主要功能:\n"
        "- 自动发现局域网内的 DLNA 播放设备\n"
        "- 实时投屏桌面画面与声音\n"
        "- 投屏本地视频、音乐与图片\n"
        "- 多网卡 SSDP 扫描与本地预览\n"
        "- 可调节帧率、码率与画面高度上限\n"
        "- 记住上次使用的投屏参数"));
    app.setApplicationAcknowledgementPage(QStringLiteral("https://gitee.com/GXDE-OS/gxde-dlna-caster"));
    app.loadTranslator();

    QIcon appIcon = QIcon::fromTheme(QStringLiteral("gxde-dlna-caster"));
    if (appIcon.isNull())
        appIcon = QIcon(QStringLiteral(":/icons/gxde-dlna-caster.png"));
    app.setWindowIcon(appIcon);
    app.setProductName(QStringLiteral("GXDE DLNA Caster"));
    app.setProductIcon(appIcon);

    MainWindow w;
    w.resize(900, 580);
    w.show();
    moveToCenter(&w);

    return app.exec();
}
