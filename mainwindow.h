#pragma once
#include <DMainWindow>
#include <DListView>
#include <DLineEdit>
#include <DPushButton>
#include <DWidget>
#include <DTitlebar>
#include <QStandardItemModel>
#include <QCheckBox>
#include "renderer.h"
#include "castcontroller.h"
#include "dlnascanner.h"

DWIDGET_USE_NAMESPACE

class QComboBox;
class QSpinBox;
class QTextEdit;
class QLabel;
class QListWidget;
class QStackedWidget;
class QGroupBox;
class QSystemTrayIcon;
class QMenu;

class MainWindow : public DMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onRefreshDevices();
    void onSourceChanged(int index);
    void onBrowseFile();
    void onCast();
    void onPreview();
    void onBrowserCast();
    void onStop();
    void onDeviceFound(const Renderer &device);
    void onScanFinished();
    void onCastStarted();
    void onCastStopped();
    void onCastError(const QString &msg);

private:
    void buildUi();
    void buildTrayIcon();
    void buildTitleMenu();
    void applyTrayEnabled();
    // 首次关闭时询问关闭行为 (最小化到托盘 / 直接退出); 返回 true 表示允许继续关闭
    bool askCloseBehavior();
    void appendLog(const QString &msg);
    void setCastingUi(bool casting);
    void updateUrlLabel();
    CastOptions collectOptions() const;
    void loadSettings();
    void saveSettings();

    DlnaScanner m_scanner;
    CastController m_controller;
    bool m_loadingSettings = false;

    // 系统托盘 (后台运行)
    QSystemTrayIcon *m_tray = nullptr;
    bool m_quitRequested = false;
    // 托盘与关闭行为设置 (QSettings 持久化)
    bool m_trayEnabled = true;    // 是否启用系统托盘
    bool m_closeToTray = true;    // 关闭窗口时最小化到托盘 (否则直接退出)
    bool m_closeAsked = false;    // 是否已询问过关闭行为
    QAction *m_trayMenuAct = nullptr;  // 标题栏菜单中托盘选项, 用于同步勾选状态

    // 侧边栏导航
    QListWidget *m_navList = nullptr;
    QStackedWidget *m_stack = nullptr;

    // 共享投屏设置
    QComboBox *m_sourceCombo = nullptr;
    DLineEdit *m_fileEdit = nullptr;
    DPushButton *m_browseBtn = nullptr;
    QSpinBox *m_fpsSpin = nullptr;
    QSpinBox *m_scaleSpin = nullptr;
    QComboBox *m_bitrateCombo = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QCheckBox *m_audioCheck = nullptr;

    // DLNA 页
    DListView *m_deviceList = nullptr;
    QStandardItemModel *m_deviceModel = nullptr;
    DPushButton *m_castBtn = nullptr;
    DPushButton *m_previewBtn = nullptr;
    DPushButton *m_refreshBtn = nullptr;

    // 浏览器投屏页
    DPushButton *m_browserCastBtn = nullptr;
    QListWidget *m_urlList = nullptr;

    // 底部操作区
    DPushButton *m_stopBtn = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTextEdit *m_logView = nullptr;
};
