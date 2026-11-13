#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
#ifdef Q_OS_WIN32
#include <Windows.h>
#endif

#include "singleinstance.h"

namespace
{
    const char kActivateMessage[] = "activate";
    const int kTimeoutMs = 1000;

    // 该名称在 Windows 上是命名管道，在其他平台是 /tmp 下的文件，
    // 因此只能使用普通字符。加入用户名，使同一机器上的不同用户各自拥有独立实例。
    QString makeServerName(const QString &appId)
    {
        QString user = qEnvironmentVariable("USERNAME");
        if (user.isEmpty()) {
            user = qEnvironmentVariable("USER");
        }
        QString name = appId + "-" + user;
        for (QChar &c : name) {
            if (!c.isLetterOrNumber() && c != '-' && c != '_') {
                c = '_';
            }
        }
        return name;
    }
}

SingleInstance::SingleInstance(const QString &appId, QObject *parent) : QObject(parent), m_serverName(makeServerName(appId))
{
    // 探测是否存在存活的主实例。崩溃的主实例残留的 socket 文件（Unix）会拒绝连接，
    // 因此连接失败意味着我们可以接管。
    QLocalSocket probe;
    probe.connectToServer(m_serverName);
    if (probe.waitForConnected(kTimeoutMs)) {
        probe.disconnectFromServer();
        m_primary = false;
        return;
    }

    QLocalServer::removeServer(m_serverName);
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (m_server->listen(m_serverName)) {
        connect(m_server, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
    } else {
        // 与其完全拒绝启动，不如以未加保护的方式运行。
        qWarning() << "SingleInstance: cannot listen on" << m_serverName << ":" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
    }
    m_primary = true;
}

SingleInstance::~SingleInstance()
{
    if (m_server) {
        m_server->close(); // 同时移除 Unix 上的 socket 文件
    }
}

bool SingleInstance::isPrimary() const
{
    return m_primary;
}

bool SingleInstance::notifyPrimary()
{
    QLocalSocket socket;
    socket.connectToServer(m_serverName);
    if (!socket.waitForConnected(kTimeoutMs)) {
        qWarning() << "SingleInstance: cannot reach the running instance:" << socket.errorString();
        return false;
    }

#ifdef Q_OS_WIN32
    // 本进程刚由用户启动，因此拥有更改前台窗口的权限。将该权限授予主实例；
    // 否则 Windows 只会闪烁任务栏按钮，而不会真正将窗口提到前台。
    AllowSetForegroundWindow(ASFW_ANY);
#endif

    socket.write(kActivateMessage);
    socket.waitForBytesWritten(kTimeoutMs);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(kTimeoutMs);
    }
    return true;
}

void SingleInstance::onNewConnection()
{
    while (QLocalSocket *client = m_server->nextPendingConnection()) {
        connect(client, &QLocalSocket::disconnected, client, &QLocalSocket::deleteLater);
        connect(client, &QLocalSocket::readyRead, this, [this, client]() {
            if (client->readAll().contains(kActivateMessage)) {
                emit activateRequested();
            }
            client->disconnectFromServer();
        });
    }
}
