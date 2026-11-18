#include "loadingoverlay.h"

#include <QEvent>
#include <QPainter>
#include <QRandomGenerator>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace {

constexpr qreal kPi = 3.14159265358979323846;

// ── 逻辑画布与构图常量（与设计稿 build_temp/loading_design_A.html 一致）──
constexpr qreal kCanvasW = 320;
constexpr qreal kCanvasH = 240;
constexpr qreal kPhoneW = 96;
constexpr qreal kPhoneH = 172;
constexpr qreal kPhoneRadius = 16;
constexpr qreal kScreenInset = 6;
constexpr qreal kScreenRadius = 10;
constexpr qreal kBand = 34;          // 粒子纵向散布半径
constexpr qreal kSpawnX = -14;       // 粒子出生 x（画布外）
constexpr qreal kParticleR1 = 3.4;   // 屏幕外粒子半径
constexpr qreal kParticleR2 = 4.0;   // 屏幕内粒子半径
constexpr int kTransitionMs = 400;   // 阶段参数插值时长
constexpr int kBreathMs = 2400;      // 阶段1 轮廓呼吸周期
constexpr int kPhase2AutoMs = 1500;  // 阶段2 自动推进到阶段3 的时长
constexpr int kFadeOutMs = 250;      // 退场淡出时长
constexpr int kDotPeriod = 1200;     // 文案弹跳点周期
constexpr int kMaxParticles = 80;    // 粒子数上限（安全阀）

const QColor kOutlineBase(58, 66, 80);     // #3A4250
const QColor kTextColor(216, 220, 224);    // #D8DCE0
// 各阶段参数表（间隔/速度/双色/拖尾/光晕/轮廓色/呼吸/特效）
struct PhaseSpec {
    qreal interval, speed, trail, glow;
    QColor c0, c1, outline;
    bool breathe;
    int fx;
};
const PhaseSpec kPhases[3] = {
    // 0 握连：稀疏慢速，纯蓝，轮廓呼吸
    { 400, 170, 0, 18, QColor(90, 180, 255), QColor(90, 180, 255), QColor(90, 180, 255), true, 0 },
    // 1 通道建立：中速，蓝→绿渐变，短拖尾，屏幕内上升微光带
    { 220, 260, 12, 20, QColor(90, 180, 255), QColor(138, 255, 193), QColor(106, 200, 255), false, 1 },
    // 2 首帧解码：快速高密度，荧光绿，长拖尾，扫光脉冲
    { 120, 420, 20, 26, QColor(138, 255, 193), QColor(138, 255, 193), QColor(138, 255, 193), false, 2 },
};

QColor lerpColor(const QColor &a, const QColor &b, qreal t)
{
    t = qBound(0.0, t, 1.0);
    return QColor(int(a.red() + (b.red() - a.red()) * t),
                  int(a.green() + (b.green() - a.green()) * t),
                  int(a.blue() + (b.blue() - a.blue()) * t),
                  int(a.alpha() + (b.alpha() - a.alpha()) * t));
}

} // namespace

LoadingOverlay::LoadingOverlay(QWidget *parent)
    : QWidget(parent)
{
    // 遮罩背景由 paintEvent 自绘（近实色深色圆角），不用样式表，
    // 保证淡出动画能连同背景一起整体渐隐
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_text = tr("Connecting to device…");
    m_clock.start();
    m_timer = new QTimer(this);
    m_timer->setInterval(16); // ≈60fps
    connect(m_timer, &QTimer::timeout, this, &LoadingOverlay::tick);
    m_timer->start();
}

void LoadingOverlay::advancePhase(int phase)
{
    phase = qBound(0, phase, 2);
    if (phase <= m_phaseTo || m_fading) {
        return; // 阶段只前进不回退；退场中不再响应
    }
    m_phaseFrom = m_phaseTo;
    m_phaseTo = phase;
    m_switchAt = m_clock.elapsed();
    if (phase == 1) {
        m_phase2At = m_switchAt;
    } else {
        m_phase2At = -1;
    }
}

void LoadingOverlay::setText(const QString &text)
{
    if (m_text == text) {
        return;
    }
    m_text = text;
    m_textIsNoFrame = false;
    update();
}

void LoadingOverlay::setNoFrameText()
{
    m_text = tr("Still no frame: make sure the device screen is on and USB debugging is authorized");
    m_textIsNoFrame = true;
    update();
}

void LoadingOverlay::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        if (m_textIsNoFrame) {
            m_text = tr("Still no frame: make sure the device screen is on and USB debugging is authorized");
        } else {
            m_text = tr("Connecting to device…");
        }
        update();
    }
}

void LoadingOverlay::fadeOutAndDelete()
{
    if (m_fading) {
        return;
    }
    m_fading = true;
    auto *anim = new QVariantAnimation(this);
    anim->setDuration(kFadeOutMs);
    anim->setStartValue(1.0);
    anim->setEndValue(0.0);
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_fade = v.toDouble();
        update();
    });
    connect(anim, &QVariantAnimation::finished, this, [this]() {
        emit fadedOut();
        deleteLater();
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

LoadingOverlay::PhaseParams LoadingOverlay::blendParams() const
{
    // 阶段切换后 400ms 内对全部动态参数做线性插值，无跳变
    const PhaseSpec &A = kPhases[m_phaseFrom];
    const PhaseSpec &B = kPhases[m_phaseTo];
    qreal k = qBound(0.0, qreal(m_clock.elapsed() - m_switchAt) / kTransitionMs, 1.0);
    const bool firstHalf = k < 0.5;
    PhaseParams p;
    p.interval = A.interval + (B.interval - A.interval) * k;
    p.speed = A.speed + (B.speed - A.speed) * k;
    p.trail = A.trail + (B.trail - A.trail) * k;
    p.glow = A.glow + (B.glow - A.glow) * k;
    p.c0 = lerpColor(A.c0, B.c0, k);
    p.c1 = lerpColor(A.c1, B.c1, k);
    p.outline = lerpColor(A.outline, B.outline, k);
    p.breathe = firstHalf ? A.breathe : B.breathe;
    p.fx = firstHalf ? A.fx : B.fx;
    return p;
}

void LoadingOverlay::tick()
{
    const qint64 now = m_clock.elapsed();
    const qreal dt = qMin(qreal(now - m_lastTick) / 1000.0, 0.05);
    m_lastTick = now;

    // 阶段2 持续 1.5s 仍未收到首帧回调 → 自动推进到阶段3（保持递进感）
    if (m_phaseTo == 1 && m_phase2At >= 0 && now - m_phase2At >= kPhase2AutoMs) {
        advancePhase(2);
    }
    if (m_fading) {
        update();
        return; // 退场中停止发射新粒子
    }

    const PhaseParams p = blendParams();

    // 按当前发射间隔生成粒子
    m_spawnAcc += dt * 1000;
    while (m_spawnAcc >= p.interval && m_particles.size() < kMaxParticles) {
        m_spawnAcc -= p.interval;
        Particle pt;
        pt.born = now;
        pt.y = kCanvasH / 2 + (QRandomGenerator::global()->generateDouble() * 2 - 1) * kBand;
        pt.seed = QRandomGenerator::global()->generateDouble();
        m_particles.append(pt);
    }
    // 剔除飞出画布的粒子
    const qreal travel = kCanvasW + 28;
    m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
                                     [now, &p, travel](const Particle &pt) {
                                         return (now - pt.born) * p.speed >= travel * 1000.0;
                                     }),
                      m_particles.end());
    update();
}

void LoadingOverlay::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(m_fade);

    const QRectF area = rect();
    // 背景：近实色深色圆角，完全盖住底下的皮肤绿色占位图
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(18, 20, 26, 246));
    painter.drawRoundedRect(area, 12, 12);

    // 逻辑画布 → 控件：等比缩放居中，上限 2.2 防止大窗口下构图过大
    qreal s = qMin(area.width() / kCanvasW, area.height() / kCanvasH);
    s = qBound(0.8, s, 2.2);
    painter.translate(area.center());
    painter.scale(s, s);
    painter.translate(-kCanvasW / 2.0, -kCanvasH / 2.0);

    const qint64 now = m_clock.elapsed();
    const PhaseParams p = blendParams();

    const QRectF phone((kCanvasW - kPhoneW) / 2.0, (kCanvasH - kPhoneH) / 2.0, kPhoneW, kPhoneH);
    const QRectF screen = phone.adjusted(kScreenInset, kScreenInset, -kScreenInset, -kScreenInset);

    // ── 手机外壳轮廓：阶段1 呼吸，其余稳定发光 ──
    qreal breathe = 1.0;
    if (p.breathe) {
        breathe = 0.5 + 0.5 * std::sin(now * 2.0 * kPi / kBreathMs);
    }
    const QColor oc = p.breathe ? lerpColor(kOutlineBase, p.outline, breathe) : p.outline;
    const qreal glow = p.glow * (p.breathe ? 0.35 + 0.65 * breathe : 1.0);
    painter.setBrush(Qt::NoBrush);
    // 多层递宽描边模拟光晕（QPainter 无 blur，宽+低透明度等效）
    for (int i = 8; i >= 1; --i) {
        QPen pen(oc);
        QColor c = oc;
        c.setAlphaF(0.10 * (1.0 - i / 9.0));
        pen.setColor(c);
        pen.setWidthF(3.0 + i * glow / 14.0);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawRoundedRect(phone, kPhoneRadius, kPhoneRadius);
    }
    QPen mainPen(oc);
    mainPen.setWidthF(3.0);
    painter.setPen(mainPen);
    painter.drawRoundedRect(phone, kPhoneRadius, kPhoneRadius);
    // 顶部听筒条
    painter.setPen(Qt::NoPen);
    QColor notch = oc;
    notch.setAlphaF(0.8);
    painter.setBrush(notch);
    painter.drawRoundedRect(QRectF(phone.center().x() - 10, phone.top() + 8, 20, 4), 2, 2);

    // ── 内屏底色：给粒子"进入屏幕"提供区域感 ──
    QColor tint(90, 140, 220);
    tint.setAlphaF(0.05);
    painter.setBrush(tint);
    painter.drawRoundedRect(screen, kScreenRadius, kScreenRadius);

    // ── 屏幕内特效 ──
    painter.setPen(Qt::NoPen);
    if (p.fx >= 1) {
        // 竖向微光带：自下而上，2s/次
        const qreal ph = (now % 2000) / 2000.0;
        const qreal by = screen.top() + screen.height() * (1.0 - ph) - 26;
        painter.save();
        painter.setClipRect(screen);
        QLinearGradient g(0, by, 0, by + 26);
        QColor band(90, 180, 255);
        band.setAlphaF(0.0);
        g.setColorAt(0, band);
        band.setAlphaF(0.12);
        g.setColorAt(0.5, band);
        band.setAlphaF(0.0);
        g.setColorAt(1, band);
        painter.setBrush(QBrush(g));
        painter.drawRect(QRectF(screen.left(), by, screen.width(), 26));
        painter.restore();
    }
    if (p.fx >= 2) {
        // 横向扫光脉冲：0.55s 扫完，1.4s 周期
        const int cyc = int(now % 1400);
        if (cyc < 550) {
            const qreal k = cyc / 550.0;
            const qreal bx = screen.left() + (screen.width() + 44) * k - 44;
            painter.save();
            painter.setClipRect(screen);
            QLinearGradient g(bx, 0, bx + 44, 0);
            QColor band(138, 255, 193);
            band.setAlphaF(0.0);
            g.setColorAt(0, band);
            band.setAlphaF(0.28);
            g.setColorAt(0.5, band);
            band.setAlphaF(0.0);
            g.setColorAt(1, band);
            painter.setBrush(QBrush(g));
            painter.drawRect(QRectF(bx, screen.top(), 44, screen.height()));
            painter.restore();
        }
    }

    // ── 数据流粒子：左→右穿过手机，屏幕内增亮偏色 ──
    const qreal travel = kCanvasW + 28;
    for (const Particle &pt : m_particles) {
        qreal prog = qMin((now - pt.born) * p.speed / (travel * 1000.0), 1.0);
        if (p.fx >= 2) {
            prog = 0.15 * prog * prog + 0.85 * prog; // 阶段3 轻微加速感
        }
        const qreal x = kSpawnX + travel * prog;
        const bool inScreen = x > screen.left() && x < screen.right();
        const QColor c = lerpColor(p.c0, p.c1, inScreen ? 0.85 : 0.25 + 0.125 * std::sin(pt.seed * 2.0 * kPi));
        qreal a = 1.0;
        if (prog < 0.12) {
            a = prog / 0.12;              // 入场 12% 渐显
        } else if (prog > 0.85) {
            a = (1.0 - prog) / 0.15;      // 出场 15% 渐隐
        }
        if (inScreen) {
            a = qMin(1.0, a * 1.3);       // 屏幕内增亮：数据写入设备
        }
        if (p.trail > 0.5) {
            QLinearGradient g(x - p.trail, 0, x, 0);
            QColor tc = c;
            tc.setAlphaF(0.0);
            g.setColorAt(0, tc);
            tc.setAlphaF(0.55 * a);
            g.setColorAt(1, tc);
            painter.setPen(QPen(QBrush(g), 3.0, Qt::SolidLine, Qt::RoundCap));
            painter.setBrush(Qt::NoBrush);
            painter.drawLine(QPointF(x - p.trail, pt.y), QPointF(x, pt.y));
        }
        const qreal r = inScreen ? kParticleR2 : kParticleR1;
        painter.setPen(Qt::NoPen);
        QColor pc = c;
        pc.setAlphaF(0.28 * a);           // 外圈柔光
        painter.setBrush(pc);
        painter.drawEllipse(QPointF(x, pt.y), r * 2.2, r * 2.2);
        pc.setAlphaF(0.95 * a);
        painter.setBrush(pc);
        painter.drawEllipse(QPointF(x, pt.y), r, r);
    }

    // ── 主文案 + 弹跳点 ──
    QFont font;
    font.setPixelSize(14);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2.0);
    painter.setFont(font);
    const QFontMetrics fm(font);
    const qreal textW = fm.horizontalAdvance(m_text);
    const qreal baseline = phone.bottom() + 18 + fm.ascent();
    const qreal textX = kCanvasW / 2.0 - (textW + 30) / 2.0;
    painter.setPen(QPen(kTextColor));
    painter.setBrush(Qt::NoBrush);
    painter.drawText(QPointF(textX, baseline), m_text);
    const qreal dotCy = baseline - fm.ascent() + 6;
    for (int i = 0; i < 3; ++i) {
        const qreal cyc = ((now + i * 150) % kDotPeriod) / qreal(kDotPeriod);
        const qreal b = cyc < 0.6 ? std::sin(cyc / 0.6 * kPi) : 0.0;
        static const QColor kDotColors[3] = { QColor(90, 180, 255), QColor(124, 196, 255), QColor(157, 212, 255) };
        QColor dc = kDotColors[i];
        dc.setAlphaF(0.45 + 0.55 * b);
        painter.setPen(Qt::NoPen);
        painter.setBrush(dc);
        painter.drawEllipse(QPointF(textX + textW + 10 + i * 13, dotCy - 8 * b), 3.5, 3.5);
    }
}
