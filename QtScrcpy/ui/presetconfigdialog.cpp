#include "presetconfigdialog.h"

#include "nowheelcontrols.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

PresetConfigDialog::PresetConfigDialog(const EncoderPreset &preset, QWidget *parent)
    : QDialog(parent)
    , m_preset(preset)
{
    setMinimumWidth(420);

    auto *mainLayout = new QVBoxLayout(this);

    // 警示标签
    m_warningLabel = new QLabel(this);
    m_warningLabel->setWordWrap(true);
    m_warningLabel->setStyleSheet(QStringLiteral("color:#e67e22; font-weight:bold; padding:8px;"));
    mainLayout->addWidget(m_warningLabel);

    // --- 编码优化档位分组 ---
    m_levelGroup = new QGroupBox(this);
    auto *levelLayout = new QGridLayout(m_levelGroup);

    m_levelLabel = new QLabel(this);
    m_levelBox = new FocusWheelComboBox(this);

    m_levelDesc = new QLabel(this);
    m_levelDesc->setWordWrap(true);
    m_levelDesc->setStyleSheet(QStringLiteral("color:#888;"));
    auto updateLevelDesc = [this](int idx) {
        if (idx >= 0 && idx < m_preset.levelCount()) {
            m_levelDesc->setText(m_preset.levels.at(idx).description);
        } else {
            m_levelDesc->clear();
        }
    };
    connect(m_levelBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, updateLevelDesc](int idx) {
                m_selectedLevel = idx;
                updateLevelDesc(idx);
                emit levelChanged(idx);
            });

    levelLayout->addWidget(m_levelLabel, 0, 0);
    levelLayout->addWidget(m_levelBox, 0, 1);
    levelLayout->addWidget(m_levelDesc, 1, 0, 1, 2);
    mainLayout->addWidget(m_levelGroup);

    // --- 关闭按钮 ---
    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::close);
    mainLayout->addWidget(buttonBox);

    retranslateUi();
    m_levelBox->setCurrentIndex(m_selectedLevel);
    updateLevelDesc(m_levelBox->currentIndex());
}

void PresetConfigDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void PresetConfigDialog::retranslateUi()
{
    // 从注册表重新取回当前语言下的预设文案（注册表在语言切换时会重建）
    const QVector<EncoderPreset> &presets = EncoderPresetRegistry::all();
    for (const EncoderPreset &candidate : presets) {
        if (candidate.id == m_preset.id) {
            m_preset = candidate;
            break;
        }
    }

    setWindowTitle(m_preset.displayName + QStringLiteral(" ") + tr("Settings"));
    m_warningLabel->setText(m_preset.warning);
    m_levelGroup->setTitle(tr("Encoding Optimization Level"));
    m_levelLabel->setText(tr("Profile:"));

    const int current = m_levelBox->currentIndex();
    m_levelBox->blockSignals(true);
    m_levelBox->clear();
    for (int i = 0; i < m_preset.levelCount(); ++i) {
        const EncoderPresetLevel &level = m_preset.levels.at(i);
        m_levelBox->addItem(level.label);
        m_levelBox->setItemData(i, level.tooltip, Qt::ToolTipRole);
    }
    if (current >= 0 && current < m_levelBox->count()) {
        m_levelBox->setCurrentIndex(current);
    }
    m_levelBox->blockSignals(false);
    if (m_levelBox->currentIndex() >= 0 && m_levelBox->currentIndex() < m_preset.levelCount()) {
        m_levelDesc->setText(m_preset.levels.at(m_levelBox->currentIndex()).description);
    } else {
        m_levelDesc->clear();
    }
}

void PresetConfigDialog::setLevel(int level)
{
    if (level >= 0 && level < m_preset.levelCount()) {
        m_selectedLevel = level;
        m_levelBox->setCurrentIndex(level);
    }
}
