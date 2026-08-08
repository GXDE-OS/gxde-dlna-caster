#include "mainwindow.h"
#include <QComboBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QLabel>
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
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(12);

    // ===== 左: 设备列表 =====
    auto *leftBox = new QVBoxLayout;
    leftBox->setSpacing(8);
    auto *leftTitle = new QLabel(tr("Devices"), central);
    leftTitle->setStyleSheet("font-weight: bold;");

    m_deviceModel = new QStandardItemModel(this);
    m_deviceList = new DListView(central);
    m_deviceList->setModel(m_deviceModel);
    m_deviceList->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto *refreshBtn = new DPushButton(tr("Refresh"), central);
    connect(refreshBtn, &DPushButton::clicked, this, &MainWindow::onRefreshDevices);

    leftBox->addWidget(leftTitle);
    leftBox->addWidget(m_deviceList, 1);
    leftBox->addWidget(refreshBtn);

    // ===== 右: 投屏设置与操作 =====
    auto *rightBox = new QVBoxLayout;
    rightBox->setSpacing(8);

    auto *optsGroup = new QGroupBox(tr("Cast Settings"), central);
    auto *form = new QFormLayout(optsGroup);
    form->setContentsMargins(12, 16, 12, 12);

    m_sourceCombo = new QComboBox(optsGroup);
    m_sourceCombo->addItem(tr("Desktop screen (live)"));
    m_sourceCombo->addItem(tr("Local media file"));
    connect(m_sourceCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSourceChanged);

    m_fileEdit = new DLineEdit(optsGroup);
    m_fileEdit->setPlaceholderText(tr("Select a video file to cast"));
    m_browseBtn = new DPushButton(tr("Browse..."), optsGroup);
    connect(m_browseBtn, &DPushButton::clicked, this, &MainWindow::onBrowseFile);
    auto *fileRow = new QHBoxLayout;
    fileRow->setContentsMargins(0, 0, 0, 0);
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
    m_scaleSpin->setSpecialValueText(tr("No scaling"));

    m_bitrateCombo = new QComboBox(optsGroup);
    m_bitrateCombo->addItems({QStringLiteral("2M"), QStringLiteral("4M"),
                              QStringLiteral("6M"), QStringLiteral("8M"),
                              QStringLiteral("12M")});
    m_bitrateCombo->setCurrentText(QStringLiteral("4M"));

    m_portSpin = new QSpinBox(optsGroup);
    m_portSpin->setRange(1024, 65535);
    m_portSpin->setValue(8090);

    m_audioCheck = new QCheckBox(tr("Capture and send desktop audio"), optsGroup);
    m_audioCheck->setChecked(true);

    form->addRow(tr("Source"), m_sourceCombo);
    form->addRow(tr("Media file"), fileRowWidget);
    form->addRow(tr("Frame rate"), m_fpsSpin);
    form->addRow(tr("Max height"), m_scaleSpin);
    form->addRow(tr("Video bitrate"), m_bitrateCombo);
    form->addRow(tr("Stream port"), m_portSpin);
    form->addRow(QString(), m_audioCheck);

    m_castBtn = new DPushButton(tr("Start Casting"), central);
    m_previewBtn = new DPushButton(tr("Preview Locally"), central);
    m_stopBtn = new DPushButton(tr("Stop Casting"), central);
    m_stopBtn->setEnabled(false);
    connect(m_castBtn, &DPushButton::clicked, this, &MainWindow::onCast);
    connect(m_previewBtn, &DPushButton::clicked, this, &MainWindow::onPreview);
    connect(m_stopBtn, &DPushButton::clicked, this, &MainWindow::onStop);

    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    btnRow->addWidget(m_castBtn);
    btnRow->addWidget(m_previewBtn);
    btnRow->addWidget(m_stopBtn);

    m_statusLabel = new QLabel(tr("Scanning devices ..."), central);
    m_statusLabel->setWordWrap(true);

    m_logView = new QTextEdit(central);
    m_logView->setReadOnly(true);
    m_logView->setMaximumHeight(160);

    rightBox->addWidget(optsGroup);
    rightBox->addLayout(btnRow);
    rightBox->addWidget(m_statusLabel);
    rightBox->addWidget(m_logView, 1);

    root->addLayout(leftBox, 3);
    root->addLayout(rightBox, 5);

    setCentralWidget(central);
    titlebar()->setTitle(tr("GXDE DLNA Caster"));

    onSourceChanged(0);
}

void MainWindow::onRefreshDevices()
{
    m_deviceModel->clear();
    m_statusLabel->setText(tr("Scanning for DLNA devices on the LAN ..."));
    appendLog(QStringLiteral("开始扫描局域网 DLNA 设备 (6 秒) ..."));
    m_scanner.startDiscovery(6000);
}

void MainWindow::onSourceChanged(int index)
{
    const bool fileMode = (index == 1);
    m_fileEdit->setEnabled(fileMode);
    m_browseBtn->setEnabled(fileMode);
}

void MainWindow::onBrowseFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select Media File"),
        QDir::homePath(),
        tr("Media files (*.mp4 *.mkv *.avi *.mov *.ts *.flv *.mp3 *.wav *.aac *.m4a);;All files (*)"));
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
        QMessageBox::warning(this, tr("Hint"),
                             tr("Please select a device on the left first.\n"
                                "You can also click \"Preview Locally\" and open the stream URL with VLC."));
        return;
    }
    const Renderer r = item->data(Qt::UserRole).value<Renderer>();
    const CastOptions o = collectOptions();
    if (!o.sourceFile.isEmpty() && !QFile::exists(o.sourceFile)) {
        QMessageBox::warning(this, tr("Hint"),
                             tr("The selected media file does not exist: %1").arg(o.sourceFile));
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
        QMessageBox::warning(this, tr("Hint"),
                             tr("The selected media file does not exist: %1").arg(o.sourceFile));
        return;
    }
    appendLog(QStringLiteral("启动本地预览 (不向设备发送指令)"));
    setCastingUi(true);
    m_controller.startCasting(Renderer(), o, true);
}

void MainWindow::onStop()
{
    m_controller.stopCasting();
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
        m_statusLabel->setText(tr("No DLNA devices found"));
        appendLog(QStringLiteral(
            "未发现 DLNA 播放设备。请依次检查:\n"
            "1. 电视/盒子与电脑在同一局域网 (同一路由器/交换机), 且电视上的 DLNA 接收端已开启 "
            "(VLC / Kodi / BubbleUPnP / 系统媒体接收器);\n"
            "2. 若电视通过 WiFi 连接, 检查路由器是否开启了 AP 隔离/客户端隔离;\n"
            "3. 确认电脑防火墙没有拦截 UDP 1900 (SSDP) 与本机流端口。\n"
            "可在下方日志里核对“SSDP 探测网卡”是否为电视所在网段。"));
    } else {
        m_statusLabel->setText(
            tr("Found %1 device(s). Select one and click \"Start Casting\".")
                .arg(m_deviceModel->rowCount()));
    }
}

void MainWindow::onCastStarted()
{
    m_statusLabel->setText(tr("Casting ... (stream: %1)")
                               .arg(m_controller.streamUrl()));
    appendLog(QStringLiteral("投屏进行中, 按“停止投屏”结束"));
}

void MainWindow::onCastStopped()
{
    m_statusLabel->setText(tr("Casting stopped"));
    setCastingUi(false);
}

void MainWindow::onCastError(const QString &msg)
{
    appendLog(QStringLiteral("[错误] %1").arg(msg));
    m_statusLabel->setText(tr("Cast failed"));
    setCastingUi(false);
    QMessageBox::warning(this, tr("Error"), msg);
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
    m_deviceList->setEnabled(!casting);
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
    m_stopBtn->setEnabled(casting);
}
