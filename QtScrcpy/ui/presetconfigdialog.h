#ifndef PRESETCONFIGDIALOG_H
#define PRESETCONFIGDIALOG_H

#include <QDialog>

#include "encoderpreset.h"

class QComboBox;

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

private:
    const EncoderPreset &m_preset;
    QComboBox *m_levelBox;
    int m_selectedLevel = 0;
};

#endif // PRESETCONFIGDIALOG_H
