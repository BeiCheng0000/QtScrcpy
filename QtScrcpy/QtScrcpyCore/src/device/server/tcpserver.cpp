#include "tcpserver.h"
#include "videosocket.h"

TcpServer::TcpServer(QObject *parent) : QTcpServer(parent) {}

TcpServer::~TcpServer() {}

void TcpServer::incomingConnection(qintptr handle)
{
    if (m_isVideoSocket) {
        VideoSocket *socket = new VideoSocket();
        socket->setSocketDescriptor(handle);
        addPendingConnection(socket);

        // 接下来是控制套接字
        m_isVideoSocket = false;
    } else {
        QTcpSocket *socket = new QTcpSocket();
        socket->setSocketDescriptor(handle);
        addPendingConnection(socket);
    }
}
