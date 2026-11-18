#ifndef GROUPCONTROLWIDGET_H
#define GROUPCONTROLWIDGET_H

#include <QWidget>
#include <QPointer>
#include <QStringList>
#include <QSet>
#include <QList>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

class QListWidget;
class QListWidgetItem;
class QLabel;
class QTextEdit;
class QPushButton;
class ApkInstallDialog;

// 群控面板：对所有已连接设备做批量操作（不依赖 host 输入广播的脆弱状态）。
// 设备勾选状态 = 本次批量操作的目标集合；勾选状态在刷新列表时保留。
class GroupControlWidget : public QWidget
{
    Q_OBJECT
public:
    explicit GroupControlWidget(QWidget *parent = nullptr);
    ~GroupControlWidget();

    // 用当前已连接设备刷新列表（保留原勾选；新设备默认不勾选）
    void syncDevices(const QStringList &serials);

    // 断开设备时清理其安装进度状态
    void deviceDisconnected(const QString &serial);
    void retranslateUi(); // 运行时语言切换

protected:
    void changeEvent(QEvent *event) override;

private slots:
    void onSelectAll();
    void onSelectNone();
    void onInvert();
    void onSetHost();
    void onBatchAction();
    void onInstallApk();
    void onApkRequested(const QStringList &paths);
    void onApkCancelled();
    void onApkProgress(const QString &serial, const QString &path, const QString &status);
    void onApkFinished(const QString &serial, int successCount, int failCount);

private:
    QStringList checkedSerials() const;
    QString displayName(const QString &serial) const;
    QString actionText(int kind) const;
    void log(const QString &line);

    QListWidget *m_deviceList = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_actionTitle = nullptr;
    QLabel *m_summary = nullptr;
    QTextEdit *m_log = nullptr;
    QPushButton *m_allBtn = nullptr;
    QPushButton *m_noneBtn = nullptr;
    QPushButton *m_invertBtn = nullptr;
    QPushButton *m_hostBtn = nullptr;
    QPushButton *m_installBtn = nullptr;
    QList<QPushButton *> m_actionBtns;
    QPointer<ApkInstallDialog> m_installDlg;

    // 每个设备一个"安装任务"：路径 -> 状态；用对象追踪完成
    struct InstallJob {
        QString serial;
        QStringList paths;
        QSet<int> done;
        int ok = 0;
        int fail = 0;
    };
    QList<InstallJob> m_installJobs;
};

#endif // GROUPCONTROLWIDGET_H
