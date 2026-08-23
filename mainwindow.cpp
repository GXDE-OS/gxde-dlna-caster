#include "mainwindow.h"
#include <QComboBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QTimer>
#include <QIcon>
#include <QStandardItem>
#include <QSettings>
#include <QClipboard>
#include <QGuiApplication>
#include <QCloseEvent>
#include <QFrame>
#include <QMenu>
#include <QAction>
#include <QSystemTrayIcon>

// 现代界面: 左侧导航 + 右侧内容区 (共享投屏设置 + 双模式页面)
//   - DLNA 投屏: 扫描局域网设备并投送画面/声音到电视
//   - 浏览器投屏: 接收方打开浏览器即可观看 (WebSocket + fMP4)

MainWindow::MainWindow(QWidget *parent)
    : DMainWindow(parent)
    , m_scanner(this)
    , m_controller(this)
{
    buildUi();

    connect(&m_scanner, &DlnaScanner::deviceFound, this, &MainWindow::onDeviceFound);
    connect(&m_scanner, &DlnaScanner::finished, this, &MainWindow::onScanFinished);
    connect(&m_scanner, &DlnaScanner::interfacesUsed, this, [this](const QStringList &ips) {
        appendLog(ips.isEmpty()
                      ? QStringLiteral("警告: 未找到可用网卡, SSDP 组播可能无法发出")
                      : QStringLiteral("SSDP 探测网卡: %1").arg(ips.join(QStringLiteral(", "))));
    });
    connect(&m_controller, &CastController::logMessage, this, &MainWindow::appendLog);
    connect(&m_controller, &CastController::castStarted, this, &MainWindow::onCastStarted);
    connect(&m_controller, &CastController::castStopped, this, &MainWindow::onCastStopped);
    connect(&m_controller, &CastController::castError, this, &MainWindow::onCastError);

    QTimer::singleShot(400, this, &MainWindow::onRefreshDevices);
}

void MainWindow::buildUi()
{
    auto *central = new DWidget(this);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ===== 左侧导航 =====
    m_navList = new QListWidget(central);
    m_navList->setFixedWidth(168);
    m_navList->setObjectName(QStringLiteral("sideNav"));
    m_navList->setFocusPolicy(Qt::NoFocus);
    m_navList->setFrameShape(QFrame::NoFrame);
    m_navList->setStyleSheet(QStringLiteral(
        "#sideNav { background: rgba(0,0,0,0.06); border: none; }"
        "#sideNav::item { height: 42px; padding-left: 20px; border-radius: 6px;"
        "  margin: 3px 8px; color: palette(text); }"
        "#sideNav::item:selected { background: rgba(0,120,255,0.18); color: palette(highlight);"
        "  font-weight: bold; }"
        "#sideNav::item:hover:!selected { background: rgba(0,0,0,0.08); }"));

    auto *navTitleItem = new QListWidgetItem(m_navList);
    auto *navTitle = new QLabel(tr("投屏模式"), m_navList);
    navTitle->setContentsMargins(8, 4, 0, 4);
    navTitle->setStyleSheet(QStringLiteral(
        "color: palette(placeholder-text); font-size: 13px; font-weight: bold;"));
    navTitleItem->setSizeHint(QSize(150, 32));
    navTitleItem->setFlags(Qt::NoItemFlags);
    m_navList->setItemWidget(navTitleItem, navTitle);

    // 标题下方的分割线
    auto *navSepItem = new QListWidgetItem(m_navList);
    auto *navSep = new QFrame(m_navList);
    navSep->setFrameShape(QFrame::HLine);
    navSep->setStyleSheet(QStringLiteral("color: rgba(128,128,128,0.35);"));
    navSepItem->setSizeHint(QSize(150, 6));
    navSepItem->setFlags(Qt::NoItemFlags);
    m_navList->setItemWidget(navSepItem, navSep);

    QListWidgetItem *navDlna = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("video-television")),
                                                   tr("DLNA 投屏"));
    QListWidgetItem *navWeb = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("applications-internet")),
                                                  tr("浏览器投屏"));
    m_navList->addItem(navDlna);
    m_navList->addItem(navWeb);
    connect(m_navList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_stack)
            m_stack->setCurrentIndex(qMax(0, row - 2));  // 0=标题 1=分割线
    });

    // ===== 右侧内容区 =====
    auto *content = new DWidget(central);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(18, 14, 18, 14);
    contentLayout->setSpacing(10);

    // ---- 顶部: 页面标题 ----
    auto *pageTitle = new QLabel(tr("GXDE 投屏工具"), content);
    pageTitle->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: bold;"));
    auto *pageSub = new QLabel(
        tr("把桌面画面与声音投送到 DLNA 电视，或让接收方用浏览器直接观看。"), content);
    pageSub->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    contentLayout->addWidget(pageTitle);
    contentLayout->addWidget(pageSub);
    contentLayout->addSpacing(4);

    // ---- 共享投屏设置 (紧凑网格) ----
    auto *optsGroup = new QGroupBox(tr("投屏设置"), content);
    auto *grid = new QGridLayout(optsGroup);
    grid->setContentsMargins(14, 18, 14, 14);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);

    const auto makeLabel = [content](const QString &text) {
        auto *l = new QLabel(text, content);
        l->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return l;
    };

    m_sourceCombo = new QComboBox(optsGroup);
    m_sourceCombo->addItem(tr("桌面画面（实时）"));
    m_sourceCombo->addItem(tr("本地媒体文件"));
    connect(m_sourceCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSourceChanged);

    m_fileEdit = new DLineEdit(optsGroup);
    m_fileEdit->setPlaceholderText(tr("选择要投屏的视频 / 音乐 / 图片文件"));
    m_browseBtn = new DPushButton(tr("浏览…"), optsGroup);
    connect(m_browseBtn, &DPushButton::clicked, this, &MainWindow::onBrowseFile);
    auto *fileRow = new QHBoxLayout;
    fileRow->setContentsMargins(0, 0, 0, 0);
    fileRow->setSpacing(6);
    fileRow->addWidget(m_fileEdit, 1);
    fileRow->addWidget(m_browseBtn);
    auto *fileRowWidget = new DWidget(optsGroup);
    fileRowWidget->setLayout(fileRow);

    m_fpsSpin = new QSpinBox(optsGroup);
    m_fpsSpin->setRange(5, 60);
    m_fpsSpin->setValue(30);
    m_fpsSpin->setSuffix(tr(" fps"));

    m_scaleSpin = new QSpinBox(optsGroup);
    m_scaleSpin->setRange(0, 2160);
    m_scaleSpin->setValue(1080);
    m_scaleSpin->setSuffix(QStringLiteral(" px"));
    m_scaleSpin->setSpecialValueText(tr("不缩放"));

    m_bitrateCombo = new QComboBox(optsGroup);
    m_bitrateCombo->addItems({QStringLiteral("2M"), QStringLiteral("4M"),
                              QStringLiteral("6M"), QStringLiteral("8M"),
                              QStringLiteral("12M")});
    m_bitrateCombo->setCurrentText(QStringLiteral("4M"));

    m_portSpin = new QSpinBox(optsGroup);
    m_portSpin->setRange(1024, 65535);
    m_portSpin->setValue(8090);

    m_audioCheck = new QCheckBox(tr("采集并发送系统声音"), optsGroup);
    m_audioCheck->setChecked(true);

    // 参数变化时自动记住
    connect(m_sourceCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { saveSettings(); });
    connect(m_fileEdit, &QLineEdit::textChanged, this, [this](const QString &) { saveSettings(); });
    connect(m_fpsSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { saveSettings(); });
    connect(m_scaleSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { saveSettings(); });
    connect(m_bitrateCombo, &QComboBox::currentTextChanged,
            this, [this](const QString &) { saveSettings(); });
    connect(m_portSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { saveSettings(); });
    connect(m_audioCheck, &QCheckBox::toggled, this, [this](bool) { saveSettings(); });

    grid->addWidget(makeLabel(tr("信号源")), 0, 0);
    grid->addWidget(m_sourceCombo, 0, 1, 1, 3);
    grid->addWidget(makeLabel(tr("媒体文件")), 1, 0);
    grid->addWidget(fileRowWidget, 1, 1, 1, 3);
    grid->addWidget(makeLabel(tr("帧率")), 2, 0);
    grid->addWidget(m_fpsSpin, 2, 1);
    grid->addWidget(makeLabel(tr("最大高度")), 2, 2);
    grid->addWidget(m_scaleSpin, 2, 3);
    grid->addWidget(makeLabel(tr("视频码率")), 3, 0);
    grid->addWidget(m_bitrateCombo, 3, 1);
    grid->addWidget(makeLabel(tr("流端口")), 3, 2);
    grid->addWidget(m_portSpin, 3, 3);
    grid->addWidget(m_audioCheck, 4, 1, 1, 3);
    contentLayout->addWidget(optsGroup);

    // ---- 双模式页面 ----
    m_stack = new QStackedWidget(content);
    m_navList->setCurrentRow(2);  // 默认选中 DLNA 投屏 (导航项行 2)

    // DLNA 页
    auto *dlnaPage = new DWidget(m_stack);
    auto *dlnaLayout = new QVBoxLayout(dlnaPage);
    dlnaLayout->setContentsMargins(0, 0, 0, 0);
    dlnaLayout->setSpacing(8);

    auto *dlnaHint = new QLabel(tr("投送到局域网内的 DLNA 播放器（电视 / 盒子 / 功放）"), dlnaPage);
    dlnaHint->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

    m_deviceModel = new QStandardItemModel(this);
    m_deviceList = new DListView(dlnaPage);
    m_deviceList->setModel(m_deviceModel);
    m_deviceList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_deviceList->setFrameShape(QFrame::NoFrame);
    m_deviceList->setMinimumHeight(220);

    m_refreshBtn = new DPushButton(tr("重新扫描"), dlnaPage);
    m_castBtn = new DPushButton(tr("开始投屏"), dlnaPage);
    m_previewBtn = new DPushButton(tr("本地预览"), dlnaPage);
    m_castBtn->setDefault(true);
    connect(m_refreshBtn, &DPushButton::clicked, this, &MainWindow::onRefreshDevices);
    connect(m_castBtn, &DPushButton::clicked, this, &MainWindow::onCast);
    connect(m_previewBtn, &DPushButton::clicked, this, &MainWindow::onPreview);

    auto *dlnaBtnRow = new QHBoxLayout;
    dlnaBtnRow->setSpacing(8);
    dlnaBtnRow->addWidget(m_refreshBtn);
    dlnaBtnRow->addStretch();
    dlnaBtnRow->addWidget(m_previewBtn);
    dlnaBtnRow->addWidget(m_castBtn);

    dlnaLayout->addWidget(dlnaHint);
    dlnaLayout->addWidget(m_deviceList, 1);
    dlnaLayout->addLayout(dlnaBtnRow);
    m_stack->addWidget(dlnaPage);

    // 浏览器投屏页
    auto *webPage = new DWidget(m_stack);
    auto *webLayout = new QVBoxLayout(webPage);
    webLayout->setContentsMargins(0, 0, 0, 0);
    webLayout->setSpacing(10);

    auto *webHint = new QLabel(
        tr("无需在接收方安装任何软件：开始投屏后，用手机 / 平板 / 电脑的浏览器\n"
           "打开下面的网址即可观看，同一局域网内可多个设备同时观看。"), webPage);
    webHint->setWordWrap(true);
    webHint->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

    m_browserCastBtn = new DPushButton(tr("开始浏览器投屏"), webPage);
    m_browserCastBtn->setMinimumHeight(44);
    m_browserCastBtn->setObjectName(QStringLiteral("browserCastBtn"));
    m_browserCastBtn->setStyleSheet(QStringLiteral(
        "#browserCastBtn { font-size: 15px; font-weight: bold; }"));
    connect(m_browserCastBtn, &DPushButton::clicked, this, [this]() {
        // 同一按钮承担"开始/停止"切换
        if (m_controller.isCasting())
            onStop();
        else
            onBrowserCast();
    });

    m_urlList = new QListWidget(webPage);
    m_urlList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_urlList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_urlList->setFrameShape(QFrame::NoFrame);
    m_urlList->setMinimumHeight(80);
    m_urlList->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: 1px dashed palette(mid);"
        "  border-radius: 8px; }"
        "QListWidget::item { height: 32px; padding-left: 8px;"
        "  color: palette(highlight); font-size: 15px; font-weight: bold;"
        "  border-radius: 6px; margin: 2px 4px; }"
        "QListWidget::item:selected { background: rgba(0,120,255,0.18); }"));

    webLayout->addWidget(webHint);
    webLayout->addWidget(m_browserCastBtn);
    webLayout->addWidget(m_urlList, 1);
    m_stack->addWidget(webPage);

    contentLayout->addWidget(m_stack, 1);

    // ---- 底部: 状态 + 停止 + 日志 ----
    m_stopBtn = new DPushButton(tr("停止投屏"), content);
    m_stopBtn->setEnabled(false);
    connect(m_stopBtn, &DPushButton::clicked, this, &MainWindow::onStop);

    m_statusLabel = new QLabel(tr("正在扫描设备…"), content);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

    auto *bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(10);
    bottomRow->addWidget(m_stopBtn);
    bottomRow->addWidget(m_statusLabel, 1);
    contentLayout->addLayout(bottomRow);

    m_logView = new QTextEdit(content);
    m_logView->setReadOnly(true);
    m_logView->setMaximumHeight(90);
    m_logView->setFrameShape(QFrame::NoFrame);
    m_logView->setStyleSheet(QStringLiteral(
        "background: rgba(0,0,0,0.05); border-radius: 6px; padding: 4px;"));
    contentLayout->addWidget(m_logView);

    // 分隔线 + 组装
    auto *separator = new QFrame(central);
    separator->setFrameShape(QFrame::VLine);
    separator->setStyleSheet(QStringLiteral("color: palette(mid);"));
    root->addWidget(m_navList);
    root->addWidget(separator);
    root->addWidget(content, 1);

    setCentralWidget(central);
    setMinimumSize(840, 640);
    setEnableWindowBackground(true);
    resize(980, 720);
    titlebar()->setTitle(tr("GXDE DLNA Caster"));

    buildTrayIcon();
    loadSettings();
    buildTitleMenu();
}

// 标题栏菜单 (DTK 自带): 提供关闭行为等全局选项 (系统托盘始终启用)
void MainWindow::buildTitleMenu()
{
    auto *menu = new QMenu(this);
    QAction *trayAct = menu->addAction(tr("关闭后隐藏到托盘"));
    trayAct->setCheckable(true);
    trayAct->setChecked(m_closeToTray);
    m_trayMenuAct = trayAct;
    connect(trayAct, &QAction::toggled, this, [this](bool on) {
        m_closeToTray = on;
        if (m_trayMenuAct)
            m_trayMenuAct->setChecked(m_closeToTray);
        saveSettings();
    });
    titlebar()->setMenu(menu);
}

// 首次关闭窗口时弹窗询问关闭行为
bool MainWindow::askCloseBehavior()
{
    QMessageBox box(this);
    box.setWindowTitle(tr("关闭窗口"));
    box.setText(tr("关闭窗口后程序将最小化到系统托盘，继续在后台运行。\n"
                   "您可以在托盘菜单中退出程序。\n\n"
                   "关闭时希望如何操作？"));
    box.setIcon(QMessageBox::Question);

    auto *remember = new QCheckBox(tr("记住我的选择，不再询问"), &box);
    box.setCheckBox(remember);

    QPushButton *toTrayBtn = box.addButton(tr("最小化到托盘"), QMessageBox::AcceptRole);
    box.addButton(tr("直接退出"), QMessageBox::RejectRole);
    QPushButton *cancelBtn = box.addButton(QMessageBox::Cancel);

    box.exec();
    const QAbstractButton *clicked = box.clickedButton();
    if (!clicked || clicked == cancelBtn)
        return false;  // 取消关闭

    m_closeToTray = (clicked == toTrayBtn);
    if (remember->isChecked())
        m_closeAsked = true;
    saveSettings();
    appendLog(QStringLiteral("关闭行为: %1").arg(m_closeToTray
                                                      ? tr("最小化到托盘")
                                                      : tr("直接退出")));
    return true;
}

// 系统托盘: 关闭窗口时最小化到后台, 可随时恢复/停止投屏/退出
void MainWindow::buildTrayIcon()
{
    m_tray = new QSystemTrayIcon(
        QIcon(QStringLiteral(":/icons/gxde-dlna-caster.png")), this);
    m_tray->setToolTip(tr("GXDE DLNA Caster - 后台运行中"));

    auto *menu = new QMenu(this);
    QAction *showAct = menu->addAction(tr("显示主窗口"));
    QAction *stopAct = menu->addAction(tr("停止投屏"));
    menu->addSeparator();
    QAction *quitAct = menu->addAction(tr("退出"));
    m_tray->setContextMenu(menu);

    connect(showAct, &QAction::triggered, this, [this]() {
        showNormal();
        raise();
        activateWindow();
    });
    connect(stopAct, &QAction::triggered, this, [this]() {
        m_controller.stopCasting();
    });
    connect(quitAct, &QAction::triggered, this, [this]() {
        m_quitRequested = true;
        m_controller.stopCasting();  // 停止投屏后再退出
        qApp->quit();
    });
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            showNormal();
            raise();
            activateWindow();
        }
    });
    m_tray->show();
}

void MainWindow::onRefreshDevices()
{
    m_deviceModel->clear();
    m_statusLabel->setText(tr("正在扫描局域网内的 DLNA 设备…"));
    appendLog(QStringLiteral("开始扫描局域网 DLNA 设备 (6 秒) ..."));
    m_scanner.startDiscovery(6000);
}

void MainWindow::onSourceChanged(int index)
{
    const bool fileMode = (index == 1);
    m_fileEdit->setEnabled(fileMode);
    m_browseBtn->setEnabled(fileMode);
}

void MainWindow::loadSettings()
{
    QSettings s;
    m_loadingSettings = true;
    m_sourceCombo->setCurrentIndex(s.value(QStringLiteral("source"), 0).toInt());
    m_fileEdit->setText(s.value(QStringLiteral("file")).toString());
    m_fpsSpin->setValue(s.value(QStringLiteral("fps"), 30).toInt());
    m_scaleSpin->setValue(s.value(QStringLiteral("scale"), 1080).toInt());
    const QString bitrate = s.value(QStringLiteral("bitrate"), QStringLiteral("4M")).toString();
    if (m_bitrateCombo->findText(bitrate) >= 0)
        m_bitrateCombo->setCurrentText(bitrate);
    m_portSpin->setValue(s.value(QStringLiteral("port"), 8090).toInt());
    m_audioCheck->setChecked(s.value(QStringLiteral("audio"), true).toBool());
    // 关闭行为设置
    m_closeToTray = s.value(QStringLiteral("close_to_tray"), true).toBool();
    m_closeAsked = s.value(QStringLiteral("close_to_tray_asked"), false).toBool();
    m_loadingSettings = false;
    onSourceChanged(m_sourceCombo->currentIndex());
}

void MainWindow::saveSettings()
{
    if (m_loadingSettings)
        return;
    QSettings s;
    s.setValue(QStringLiteral("source"), m_sourceCombo->currentIndex());
    s.setValue(QStringLiteral("file"), m_fileEdit->text());
    s.setValue(QStringLiteral("fps"), m_fpsSpin->value());
    s.setValue(QStringLiteral("scale"), m_scaleSpin->value());
    s.setValue(QStringLiteral("bitrate"), m_bitrateCombo->currentText());
    s.setValue(QStringLiteral("port"), m_portSpin->value());
    s.setValue(QStringLiteral("audio"), m_audioCheck->isChecked());
    s.setValue(QStringLiteral("close_to_tray"), m_closeToTray);
    s.setValue(QStringLiteral("close_to_tray_asked"), m_closeAsked);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();

    // 从托盘菜单"退出"或系统退出: 直接退出
    if (m_quitRequested) {
        m_controller.stopCasting();
        DMainWindow::closeEvent(event);
        return;
    }

    // 系统托盘始终启用; 首次关闭时弹窗询问关闭行为 (最小化到托盘 / 直接退出)
    if (!m_closeAsked && !askCloseBehavior()) {
        event->ignore();  // 用户取消关闭
        return;
    }
    // 同步标题栏菜单勾选状态 (首次询问可能改变了关闭行为)
    if (m_trayMenuAct)
        m_trayMenuAct->setChecked(m_closeToTray);

    if (m_closeToTray) {
        hide();
        m_tray->showMessage(tr("GXDE 投屏工具"),
                            tr("已最小化到系统托盘，点击托盘图标可重新打开。"));
        event->ignore();
        return;
    }

    m_controller.stopCasting();
    DMainWindow::closeEvent(event);
}

void MainWindow::onBrowseFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("选择媒体文件"),
        QDir::homePath(),
        tr("媒体文件 (*.mp4 *.mkv *.avi *.mov *.ts *.flv *.webm *.mp3 *.wav *.flac *.aac *.m4a *.ogg *.jpg *.jpeg *.png *.bmp *.gif *.webp);;所有文件 (*)"));
    if (!path.isEmpty())
        m_fileEdit->setText(path);
}

CastOptions MainWindow::collectOptions() const
{
    CastOptions o;
    if (m_sourceCombo->currentIndex() == 1)
        o.sourceFile = m_fileEdit->text().trimmed();
    o.fps = m_fpsSpin->value();
    o.scale = m_scaleSpin->value();
    o.bitrate = m_bitrateCombo->currentText();
    o.audio = m_audioCheck->isChecked();
    o.port = quint16(m_portSpin->value());
    return o;
}

void MainWindow::onCast()
{
    if (m_controller.isCasting())
        return;

    const QStandardItem *item = m_deviceModel->item(m_deviceList->currentIndex().row());
    if (!item) {
        QMessageBox::warning(this, tr("提示"),
                             tr("请先在左侧设备列表中选择一台设备。\n"
                                "也可以点击“本地预览”，用 VLC 打开流地址查看。"));
        return;
    }
    const Renderer r = item->data(Qt::UserRole).value<Renderer>();
    const CastOptions o = collectOptions();
    if (!o.sourceFile.isEmpty() && !QFile::exists(o.sourceFile)) {
        QMessageBox::warning(this, tr("提示"),
                             tr("所选媒体文件不存在: %1").arg(o.sourceFile));
        return;
    }

    appendLog(QStringLiteral("准备投屏到: %1").arg(r.name));
    setCastingUi(true);
    m_controller.startCasting(r, o, false);
}

void MainWindow::onPreview()
{
    if (m_controller.isCasting())
        return;
    const CastOptions o = collectOptions();
    if (!o.sourceFile.isEmpty() && !QFile::exists(o.sourceFile)) {
        QMessageBox::warning(this, tr("提示"),
                             tr("所选媒体文件不存在: %1").arg(o.sourceFile));
        return;
    }
    appendLog(QStringLiteral("启动本地预览 (不向设备发送指令)"));
    setCastingUi(true);
    m_controller.startCasting(Renderer(), o, true);
}

void MainWindow::onBrowserCast()
{
    if (m_controller.isCasting())
        return;
    const CastOptions o = collectOptions();
    if (!o.sourceFile.isEmpty() && !QFile::exists(o.sourceFile)) {
        QMessageBox::warning(this, tr("提示"),
                             tr("所选媒体文件不存在: %1").arg(o.sourceFile));
        return;
    }
    appendLog(QStringLiteral("启动浏览器投屏 ..."));
    setCastingUi(true);
    m_controller.startBrowserCasting(o);
}

void MainWindow::onStop()
{
    m_controller.stopCasting();
}

void MainWindow::updateUrlLabel()
{
    m_urlList->clear();
    if (m_controller.isBrowserMode()) {
        const QStringList urls = m_controller.browserUrls();
        for (const QString &u : urls) {
            // 每条 URL 一个条目, 自带"复制"按钮
            auto *item = new QListWidgetItem(m_urlList);
            item->setSizeHint(QSize(0, 38));
            item->setFlags(Qt::ItemIsEnabled);

            auto *row = new QWidget(m_urlList);
            auto *lay = new QHBoxLayout(row);
            lay->setContentsMargins(10, 0, 4, 0);
            lay->setSpacing(6);

            auto *label = new QLabel(u, row);
            label->setStyleSheet(QStringLiteral(
                "color: palette(highlight); font-size: 14px; font-weight: bold;"
                " background: transparent;"));
            auto *btn = new DPushButton(tr("复制"), row);
            btn->setFixedSize(56, 26);
            connect(btn, &DPushButton::clicked, this, [this, u]() {
                QGuiApplication::clipboard()->setText(u);
                appendLog(QStringLiteral("已复制网址: %1").arg(u));
            });

            lay->addWidget(label, 1);
            lay->addWidget(btn);
            m_urlList->setItemWidget(item, row);
        }
    } else {
        auto *hint = new QListWidgetItem(tr("尚未开始投屏"));
        hint->setTextAlignment(Qt::AlignCenter);
        hint->setFlags(Qt::NoItemFlags);
        m_urlList->addItem(hint);
    }
}

void MainWindow::onDeviceFound(const Renderer &device)
{
    auto *item = new QStandardItem(
        QIcon::fromTheme(QStringLiteral("video-television")), device.name);
    item->setData(QVariant::fromValue(device), Qt::UserRole);
    item->setToolTip(device.controlUrl);
    m_deviceModel->appendRow(item);
    appendLog(QStringLiteral("发现设备: %1").arg(device.name));
}

void MainWindow::onScanFinished()
{
    if (m_deviceModel->rowCount() == 0) {
        m_statusLabel->setText(tr("未发现 DLNA 设备"));
        appendLog(QStringLiteral(
            "未发现 DLNA 播放设备。请依次检查:\n"
            "1. 电视/盒子与电脑在同一局域网, 且电视上的 DLNA 接收端已开启 "
            "(VLC / Kodi / BubbleUPnP / 系统媒体接收器);\n"
            "2. 若电视通过 WiFi 连接, 检查路由器是否开启了 AP 隔离/客户端隔离;\n"
            "3. 确认电脑防火墙没有拦截 UDP 1900 (SSDP) 与本机流端口。\n"
            "可在下方日志里核对“SSDP 探测网卡”是否为电视所在网段。"));
    } else {
        m_statusLabel->setText(
            tr("发现 %1 台设备，选择后点击“开始投屏”。")
                .arg(m_deviceModel->rowCount()));
    }
}

void MainWindow::onCastStarted()
{
    updateUrlLabel();
    if (m_controller.isBrowserMode()) {
        m_statusLabel->setText(tr("浏览器投屏进行中，接收方打开上面的网址即可观看"));
        appendLog(QStringLiteral("浏览器投屏进行中, 按“停止投屏”结束"));
        m_navList->setCurrentRow(3);  // 浏览器投屏页 (0=标题 1=分割线 2=DLNA 3=浏览器)
    } else {
        m_statusLabel->setText(tr("投屏进行中: %1").arg(m_controller.streamUrl()));
        appendLog(QStringLiteral("投屏进行中, 按“停止投屏”结束"));
    }
}

void MainWindow::onCastStopped()
{
    updateUrlLabel();
    m_statusLabel->setText(tr("投屏已停止"));
    setCastingUi(false);
}

void MainWindow::onCastError(const QString &msg)
{
    appendLog(QStringLiteral("[错误] %1").arg(msg));
    updateUrlLabel();
    m_statusLabel->setText(tr("投屏失败"));
    setCastingUi(false);
    QMessageBox::warning(this, tr("错误"), msg);
}

void MainWindow::appendLog(const QString &msg)
{
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                  msg);
    m_logView->append(line);
}

void MainWindow::setCastingUi(bool casting)
{
    m_navList->setEnabled(!casting);
    m_sourceCombo->setEnabled(!casting);
    const bool fileMode = !casting && m_sourceCombo->currentIndex() == 1;
    m_fileEdit->setEnabled(fileMode);
    m_browseBtn->setEnabled(fileMode);
    m_fpsSpin->setEnabled(!casting);
    m_scaleSpin->setEnabled(!casting);
    m_bitrateCombo->setEnabled(!casting);
    m_portSpin->setEnabled(!casting);
    m_audioCheck->setEnabled(!casting);
    m_castBtn->setEnabled(!casting);
    m_previewBtn->setEnabled(!casting);
    m_refreshBtn->setEnabled(!casting);
    // 浏览器投屏按钮承担"开始/停止"切换: 投屏中保持可点 (点击 = 停止)
    m_browserCastBtn->setEnabled(true);
    m_browserCastBtn->setText(casting ? tr("停止浏览器投屏") : tr("开始浏览器投屏"));
    m_stopBtn->setEnabled(casting);
}
