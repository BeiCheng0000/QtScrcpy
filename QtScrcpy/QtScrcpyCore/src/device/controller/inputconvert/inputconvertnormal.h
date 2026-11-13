#ifndef INPUTCONVERT_H
#define INPUTCONVERT_H

#include "inputconvertbase.h"

class InputConvertNormal : public InputConvertBase
{
    Q_OBJECT
public:
    InputConvertNormal(Controller *controller);
    virtual ~InputConvertNormal();

    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize);

private:
    AndroidMotioneventButtons convertMouseButtons(Qt::MouseButtons buttonState);
    AndroidMotioneventButtons convertMouseButton(Qt::MouseButton button);
    AndroidKeycode convertKeyCode(int key, Qt::KeyboardModifiers modifiers);
    AndroidMetastate convertMetastate(Qt::KeyboardModifiers modifiers);

    // 双指缩放 / 倾斜模拟（移植自 scrcpy）
    // 按住 Ctrl 并拖动左键：会在屏幕中心另一侧对称的位置
    // 注入一个“虚拟手指”，Android 应用会将其识别为双指捏合。
    //   Ctrl        -> 双指缩放 / 旋转（两个轴都反转）
    //   Shift       -> 垂直倾斜（仅反转 x 轴）
    //   Ctrl+Shift  -> 水平倾斜（仅反转 y 轴）
    void sendVirtualFingerEvent(AndroidMotioneventAction action, const QPoint &pos,
                                const QSize &frameSize);

    bool m_vfingerDown = false;
    bool m_vfingerInvertX = false;
    bool m_vfingerInvertY = false;
};

#endif // INPUTCONVERT_H
