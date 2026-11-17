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

    QLineEdit *m_pathEdit = nullptr;
    QListWidget *m_list = nullptr;
    QCheckBox *m_recursive = nullptr;
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QLabel *m_summary = nullptr;
    bool m_busy = false;
};

#endif // APKINSTALLDIALOG_H
