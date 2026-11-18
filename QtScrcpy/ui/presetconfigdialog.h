#ifndef PRESETCONFIGDIALOG_H
#define PRESETCONFIGDIALOG_H

#include <QDialog>

#include "encoderpreset.h"

class QComboBox;
class QDialogButtonBox;
class QGroupBox;
class QLabel;

// 用于配置单个 EncoderPreset 的通用对话框。它会渲染预设的警示文本与优化档位，
// 因此任何已注册的预设都无需专门编写对话框代码即可编辑。
class PresetConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PresetConfigDialog(const EncoderPreset &preset, QWidget *parent = nullptr);

    int selectedLevel() const { return m_selectedLevel; }
    void setLevel(int level);
    QString presetId() const { return m_preset.id; }

signals:
    void levelChanged(int level);

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslateUi();

    EncoderPreset m_preset; // 值拷贝：语言切换会重建注册表，引用会悬空
    QLabel *m_warningLabel = nullptr;
    QGroupBox *m_levelGroup = nullptr;
    QLabel *m_levelLabel = nullptr;
    QComboBox *m_levelBox = nullptr;
    QLabel *m_levelDesc = nullptr;
    int m_selectedLevel = 0;
};

#endif // PRESETCONFIGDIALOG_H
