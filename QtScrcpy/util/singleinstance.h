#ifndef SINGLEINSTANCE_H
#define SINGLEINSTANCE_H

#include <QObject>
#include <QString>

class QLocalServer;

// 保证每个用户只运行一个 QtScrcpy 进程。
//
// 第一个进程成为主实例并监听本地 socket。之后启动的进程会发现该 socket，
// 请求主实例显示其主窗口（可能隐藏在系统托盘中），随后退出。
class SingleInstance : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstance(const QString &appId, QObject *parent = nullptr);
    ~SingleInstance() override;

    // 未找到其他实例、本进程当前持有锁时为 true
    bool isPrimary() const;

    // 仅次要实例使用：请求主实例显示其窗口
    bool notifyPrimary();

signals:
    // 仅主实例使用：有次要实例请求我们显示窗口
    void activateRequested();

private:
    void onNewConnection();

private:
    QString m_serverName;
    QLocalServer *m_server = nullptr;
    bool m_primary = false;
};

#endif // SINGLEINSTANCE_H
