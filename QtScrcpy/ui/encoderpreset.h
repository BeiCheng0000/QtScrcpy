#ifndef ENCODERPRESET_H
#define ENCODERPRESET_H

#include <QString>
#include <QVector>

// 预设中的单个优化档位（例如 MTK 的“游戏模式”/“均衡”/“省电”）。
struct EncoderPresetLevel
{
    QString label;        // 下拉项显示文本
    QString tooltip;      // 下拉项悬停提示
    QString description;  // 下拉框下方展示的详细说明
    QString options;      // 该档位对应的完整 codec-options 字符串
    quint32 bitRate = 0;  // 0 = leave the user bitrate untouched
    int maxSizeIndex = -1; // -1 = this tier does not prescribe a max size
};

// A vendor/scenario-specific encoder preset. A preset owns a fixed codec
// (codecName) plus one or more optimization tiers. Presets are plain data so
// 新增预设只需构造一个实例并注册即可。
struct EncoderPreset
{
    QString id;            // 稳定标识符，例如 "mtk"
    QString displayName;   // 显示在编码模式下拉框中的名称
    QString codecName;     // 为空表示不覆盖设备自带编解码器
    QString warning;       // 对话框顶部展示的警示文本
    QVector<EncoderPresetLevel> levels;

    int levelCount() const { return levels.size(); }
    quint32 levelBitRate(int level) const;
    int levelMaxSizeIndex(int level) const;
    void buildParams(int level, QString &outOptions, quint32 &outBitRate) const;
};

class EncoderPresetRegistry
{
public:
    static const QVector<EncoderPreset> &all();
};

#endif // ENCODERPRESET_H
