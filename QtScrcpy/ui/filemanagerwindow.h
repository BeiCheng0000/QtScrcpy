#ifndef FILEMANAGERWINDOW_H
#define FILEMANAGERWINDOW_H

#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "../QtScrcpyCore/src/device/filemanager/filemanager.h"

class QDialog;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTableView;
class QToolButton;

/// 单设备文件管理窗口：浏览 / 上传（拖拽）/ 下载 / 编辑保存 / 刷新 / 历史导航 / 跨目录跳转
class FileManagerWindow : public QWidget
{
    Q_OBJECT
public:
    explicit FileManagerWindow(const QString &serial, QWidget *parent = nullptr);
    ~FileManagerWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void buildUi();
    void retranslateUi();
    void navigateTo(const QString &path, bool recordHistory = true);
    void refresh();
    void goBack();
    void goForward();
    void goUp();
    void updateNavState();
    void rebuildBreadcrumb(const QString &path);
    void applyEntries(const QVector<qsc::DeviceFileEntry> &entries);
    void setBusy(bool busy);
    void showError(const QString &title, const QString &text);

    QStringList selectedPaths() const;
    QString dropTargetDir(const QPoint &pos) const;
    bool isEditableName(const QString &name) const;
    static QString typeText(const qsc::DeviceFileEntry &entry);
    static QString formatSize(qint64 bytes);
    static QString formatTime(qint64 epochSec);

    void startUpload(const QStringList &localFiles, const QString &deviceDir);
    void uploadViaDialog();
    void downloadSelected();
    void confirmDelete(const QStringList &devicePaths);
    void openEditor(const QString &path);
    void showContextMenu(const QPoint &pos);
    void retrySaveWithForce();

private slots:
    void onDirListed(const QString &path, const QVector<qsc::DeviceFileEntry> &entries);
    void onListFailed(const QString &path, const QString &reason);
    void onTransferStarted(int total);
    void onTransferProgress(int index, const QString &name, qint64 done, qint64 total);
    void onTransferItemFinished(int index, const QString &name, bool ok, const QString &error);
    void onTransferFinished(int succeeded, int failed, bool canceled);
    void onTextFileLoaded(const QString &path, const QString &content,
                          const QString &encoding, qint64 mtime);
    void onTextFileLoadFailed(const QString &path, const QString &reason);
    void onTextFileSaved(const QString &path, bool ok, const QString &error);
    void onDeleteFinished(const QStringList &paths, bool ok, const QString &error);

private:
    QString m_serial;
    qsc::FileManager *m_manager = nullptr;

    QTableView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QWidget *m_crumbWidget = nullptr;
    QHBoxLayout *m_crumbLayout = nullptr;
    QToolButton *m_backBtn = nullptr;
    QToolButton *m_forwardBtn = nullptr;
    QToolButton *m_upBtn = nullptr;
    QToolButton *m_refreshBtn = nullptr;
    QToolButton *m_uploadBtn = nullptr;
    QToolButton *m_downloadBtn = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progress = nullptr;

    QVector<QString> m_history;
    int m_historyPos = -1;
    QString m_currentPath;
    int m_pendingTransfers = 0;
    QStringList m_transferErrors;

    // 编辑器（同一时刻只允许一个）
    QPointer<QDialog> m_editor;
    QPlainTextEdit *m_editorText = nullptr;
    QString m_editorPath;
    QString m_editorEncoding;
    qint64 m_editorMtime = 0;
    bool m_editorSaveForce = false;
};

#endif // FILEMANAGERWINDOW_H
