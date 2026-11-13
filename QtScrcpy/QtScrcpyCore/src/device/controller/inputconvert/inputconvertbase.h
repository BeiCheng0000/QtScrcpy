#ifndef INPUTCONVERTBASE_H
#define INPUTCONVERTBASE_H

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QWheelEvent>

#include "controlmsg.h"

class Controller;
class InputConvertBase : public QObject
{
    Q_OBJECT
public:
    InputConvertBase(Controller *controller);
    virtual ~InputConvertBase();

    // 帧尺寸可能与设备真实尺寸不同，因此我们需要
    // 绝对坐标所基于的尺寸，以便相应地缩放
    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize) = 0;
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize) = 0;
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize) = 0;
    virtual bool isCurrentCustomKeymap()
    {
        return false;
    }

signals:
    void grabCursor(bool grab);

protected:
    void sendControlMsg(ControlMsg *msg);

    QPointer<Controller> m_controller;
    // Qt 把重复事件以布尔值上报，但 Android 期望的是实际的
    // 重复次数。该变量用于记录这个计数。
    unsigned m_repeat = 0;
};

#endif // INPUTCONVERTBASE_H
