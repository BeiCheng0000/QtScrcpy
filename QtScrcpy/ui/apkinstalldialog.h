#ifndef APKINSTALLDIALOG_H
#define APKINSTALLDIALOG_H

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// 批量安装 APK 对话框：支持拖入文件/目录、手工输入路径（含 UNC 网络路径）、
// 文件/目录选择对话框；路径统一展开为 apk 列表后交给 VideoForm 串行安装。
class ApkInstallDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ApkInstallDialog(QWidget *parent = nullptr);

public slots:
    void setItemStatus(const QString &path, const QString &status);
    void finish(int successCount, int failCount);

signals:
    void installRequested(const QStringList &paths);
    void cancelled();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override; // 运行时语言切换

private slots:
    void addFromInput();
    void browseFiles();
    void browseDirs();
    void removeSelected();
    void startInstall();
    void cancelInstall();

private:
    void addPaths(const QStringList &rawPaths);
    void setBusy(bool busy);
    void retranslateUi();
    void refreshSummary();
    void refreshItemTexts();

    QLabel *m_hintLabel = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_addBtn = nullptr;
    QPushButton *m_fileBtn = nullptr;
    QPushButton *m_dirBtn = nullptr;
    QListWidget *m_list = nullptr;
    QCheckBox *m_recursive = nullptr;
    QPushButton *m_removeBtn = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    bool m_busy = false;
    int m_successCount = 0;
    int m_failCount = 0;
    enum class SummaryMode { None, Total, Installing, Finished, Cancelled };
    SummaryMode m_summaryMode = SummaryMode::None;
};

#endif // APKINSTALLDIALOG_H
