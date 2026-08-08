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
    void onStop();
    void onDeviceFound(const Renderer &device);
    void onScanFinished();
    void onCastStarted();
    void onCastStopped();
    void onCastError(const QString &msg);

private:
    void buildUi();
    void appendLog(const QString &msg);
    void setCastingUi(bool casting);
    CastOptions collectOptions() const;
    void loadSettings();
    void saveSettings();

    DlnaScanner m_scanner;
    CastController m_controller;
    bool m_loadingSettings = false;
    DListView *m_deviceList = nullptr;
    QStandardItemModel *m_deviceModel = nullptr;
    QComboBox *m_sourceCombo = nullptr;
    DLineEdit *m_fileEdit = nullptr;
    DPushButton *m_browseBtn = nullptr;
    QSpinBox *m_fpsSpin = nullptr;
    QSpinBox *m_scaleSpin = nullptr;
    QComboBox *m_bitrateCombo = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QCheckBox *m_audioCheck = nullptr;
    DPushButton *m_castBtn = nullptr;
    DPushButton *m_previewBtn = nullptr;
    DPushButton *m_stopBtn = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTextEdit *m_logView = nullptr;
};
