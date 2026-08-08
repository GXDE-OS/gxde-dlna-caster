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
    app.setApplicationVersion(QStringLiteral("1.0.0"));
    app.loadTranslator();
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("gxde-dlna-caster")));

    MainWindow w;
    w.resize(900, 580);
    w.show();
    moveToCenter(&w);

    return app.exec();
}
