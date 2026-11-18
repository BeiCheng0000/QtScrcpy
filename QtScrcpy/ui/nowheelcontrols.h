#pragma once

#include <QComboBox>
#include <QSpinBox>
#include <QWheelEvent>

// 滚轮彻底禁用的 SpinBox / ComboBox。
// 需求明确：滚轮永远不得改变输入参数（无论控件是否有焦点）。
// 全局兜底见 main.cpp 的 WheelFocusGuard（覆盖 .ui 静态控件与内部
// QLineEdit 路径）；本类在控件层面提供同样的保证——滚轮一律 ignore()
// 冒泡给父级（QScrollArea 继续滚动页面），绝不调用基类改值逻辑。
// 两层防护一致，任一层存在时滚轮都无法改值。
class FocusWheelSpinBox : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};

class FocusWheelComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};
