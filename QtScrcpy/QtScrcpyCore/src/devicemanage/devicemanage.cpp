#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include "devicemanage.h"
#include "device.h"
#include "demuxer.h"

namespace qsc {

#define DM_MAX_DEVICES_NUM 1000

IDeviceManage& IDeviceManage::getInstance() {
    static DeviceManage dm;
    return dm;
}

DeviceManage::DeviceManage() {
    Demuxer::init();
}

DeviceManage::~DeviceManage() {
    Demuxer::deInit();
}

QPointer<IDevice> DeviceManage::getDevice(const QString &serial)
{
    if (!m_devices.contains(serial)) {
        return QPointer<IDevice>();
    }
    return m_devices[serial];
}

bool DeviceManage::connectDevice(qsc::DeviceParams params)
{
    if (params.serial.trimmed().isEmpty()) {
        return false;
    }
    if (m_devices.contains(params.serial)) {
        return false;
    }
    if (DM_MAX_DEVICES_NUM < m_devices.size()) {
        qInfo("over the maximum number of connections");
        return false;
    }
    // 为每台设备分配独立的本地端口：reverse 模式下 serverSocket 会持续
    // listen 该端口（不是“连接建立后释放”），多台设备都用默认 27183 时
    // 第二台 listen 必失败；forward 模式的 adb forward 也是全局端口注册，
    // 同端口多设备同样不可区分。因此统一从 27183 起分配空闲端口。
    {
        const quint16 port = getFreePort();
        if (0 == port) {
            qInfo("no free local port, fall back to default %d", params.localPort);
        } else {
            params.localPort = port;
            qInfo("assign local port %d to %s", port, params.serial.toUtf8().constData());
        }
    }
    IDevice *device = new Device(params);
    connect(device, &Device::deviceConnected, this, &DeviceManage::onDeviceConnected);
    connect(device, &Device::deviceDisconnected, this, &DeviceManage::onDeviceDisconnected);
    if (!device->connectDevice()) {
        delete device;
        return false;
    }
    m_devices[params.serial] = device;
    return true;
}

bool DeviceManage::disconnectDevice(const QString &serial)
{
    bool ret = false;
    if (!serial.isEmpty() && m_devices.contains(serial)) {
        auto it = m_devices.find(serial);
        if (it->data()) {
            delete it->data();
            ret = true;
        }
    }
    return ret;
}

void DeviceManage::disconnectAllDevice()
{
    QMapIterator<QString, QPointer<IDevice>> i(m_devices);
    while (i.hasNext()) {
        i.next();
        if (i.value()) {
            delete i.value();
        }
    }
}

void DeviceManage::onDeviceConnected(bool success, const QString &serial, const QString &deviceName, const QSize &size)
{
    emit deviceConnected(success, serial, deviceName, size);
    if (!success) {
        removeDevice(serial);
    }
}

void DeviceManage::onDeviceDisconnected(QString serial)
{
    emit deviceDisconnected(serial);
    removeDevice(serial);
}

quint16 DeviceManage::getFreePort()
{
    quint16 port = m_localPortStart;
    while (port < m_localPortStart + DM_MAX_DEVICES_NUM) {
        bool used = false;
        QMapIterator<QString, QPointer<IDevice>> i(m_devices);
        while (i.hasNext()) {
            i.next();
            auto device = i.value();
            if (device && device->isReversePort(port)) {
                used = true;
                break;
            }
        }
        if (!used) {
            return port;
        }
        port++;
    }
    return 0;
}

void DeviceManage::removeDevice(const QString &serial)
{
    if (!serial.isEmpty() && m_devices.contains(serial)) {
        m_devices[serial]->deleteLater();
        m_devices.remove(serial);
    }
}

}
