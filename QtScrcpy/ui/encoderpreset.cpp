#include "encoderpreset.h"

#include <QCoreApplication>

namespace {

// 构建联发科（MediaTek）编码预设。字符串使用稳定的 “PresetConfigDialog”
// 上下文标记以便翻译，这样无论代码位于哪个文件，lupdate 都能持续收集它们。
EncoderPreset makeMtkPreset()
{
    EncoderPreset preset;
    preset.id = QStringLiteral("mtk");
    preset.displayName = QCoreApplication::translate("PresetConfigDialog", "MTK Encoder");
    preset.codecName = QStringLiteral("c2.mtk.avc.encoder");
    preset.warning = QCoreApplication::translate("PresetConfigDialog",
        "This feature is for MediaTek (MTK) devices only.\n"
        "Using on non-MTK devices may cause encoding failure or visual artifacts.");

    // 三个档位均使用 VBR（bitrate-mode=1）——c2.mtk.avc.encoder 不支持 CBR
    //（已在 MT6895、Android 13 上确认）。
    //
    // 档位 0 “游戏模式”：严格 QP 上限（28）+ 短 GOP（5）
    //   硬件 QP 上限从物理层面阻止 Sticky QP 劣化画面。即使编码器码率控制
    //   试图抬高 QP，也无法超过 28——画面始终保持清晰。
    static const QString gameMode = QStringLiteral(
        "profile=1,max-bframes=0,i-frame-interval=5,priority=0,bitrate-mode=1,"
        "video-qp-min=15,video-qp-max=28");

    // 档位 1 “均衡” / 档位 2 “省电”：标准 QP 上限（35）
    static const QString common = QStringLiteral(
        "profile=1,max-bframes=0,i-frame-interval=10,priority=0,bitrate-mode=1,"
        "video-qp-max=35,"
        "vendor.mtk.ext.venc.i.frame.control.size.max-i-ratio:int=30,"
        "vendor.mtk.ext.venc.mbrc.tracking-speed:int=0");

    // 档位 2 的额外项：关闭画质优化、低功耗运动估计、单参考帧
    static const QString visualOpt = QStringLiteral(
        "vendor.mtk.ext.venc.visual.rd:int=0,vendor.mtk.ext.venc.visual.quant:int=0");
    static const QString meLowPower = QStringLiteral(
        "vendor.mtk.ext.venc.highquality.feature-on:int=1,"
        "vendor.mtk.ext.venc.highquality.mode:int=0");
    static const QString extreme = QStringLiteral(
        "vendor.mtk.ext.venc.ref.frame.num:int=1,"
        "vendor.mtk.venc.dynamic.qpbound.min:int=28,"
        "vendor.mtk.venc.dynamic.qpbound.max:int=51");

    EncoderPresetLevel level0;
    level0.label = QCoreApplication::translate("PresetConfigDialog", "Game Mode (20Mbps, QP≤28)");
    level0.tooltip = QCoreApplication::translate("PresetConfigDialog", "VBR + hard QP cap at 28 + short GOP. The strongest anti-blur shield for real-time gaming.");
    level0.description = QCoreApplication::translate("PresetConfigDialog", "VBR + QP cap at 28 + short GOP (5 frames). Hard ceiling blocks Sticky QP completely — best for real-time gaming.");
    level0.options = gameMode;
    level0.bitRate = 20000000;

    EncoderPresetLevel level1;
    level1.label = QCoreApplication::translate("PresetConfigDialog", "Balanced (8Mbps, QP≤35)");
    level1.tooltip = QCoreApplication::translate("PresetConfigDialog", "Standard anti-blur protection with optimal visual quality. Recommended for daily use.");
    level1.description = QCoreApplication::translate("PresetConfigDialog", "VBR + QP cap at 35 + standard GOP. Matches default visual quality while preventing QP runaway. Ideal for daily use.");
    level1.options = common;
    level1.bitRate = 8000000;

    EncoderPresetLevel level2;
    level2.label = QCoreApplication::translate("PresetConfigDialog", "Power Saver (4Mbps, QP≤35)");
    level2.tooltip = QCoreApplication::translate("PresetConfigDialog", "Maximum power saving: low-power ME + single ref frame. Saves most encoder power for non-gaming scenarios.");
    level2.description = QCoreApplication::translate("PresetConfigDialog", "VBR + QP cap at 35 + disabled visual optimizations + low-power ME + single ref frame. Maximum power saving for non-intensive scenarios.");
    level2.options = common + QStringLiteral(",") + visualOpt
                     + QStringLiteral(",") + meLowPower
                     + QStringLiteral(",") + extreme;
    level2.bitRate = 4000000;

    preset.levels = { level0, level1, level2 };
    return preset;
}

} // namespace

const QVector<EncoderPreset> &EncoderPresetRegistry::all()
{
    static const QVector<EncoderPreset> presets = { makeMtkPreset() };
    return presets;
}

quint32 EncoderPreset::levelBitRate(int level) const
{
    return (level >= 0 && level < levels.size()) ? levels.at(level).bitRate : 0;
}

int EncoderPreset::levelMaxSizeIndex(int level) const
{
    return (level >= 0 && level < levels.size()) ? levels.at(level).maxSizeIndex : -1;
}

void EncoderPreset::buildParams(int level, QString &outOptions, quint32 &outBitRate) const
{
    if (level >= 0 && level < levels.size()) {
        outOptions = levels.at(level).options;
        outBitRate = levels.at(level).bitRate;
    } else {
        outOptions.clear();
        outBitRate = 0;
    }
}
