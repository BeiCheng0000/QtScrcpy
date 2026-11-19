#include "mousetrackrecorder.h"

#include <QFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QRadialGradient>
#include <QTransform>
#include <QWheelEvent>
#include <QtMath>

namespace
{
const char *eventTypeString(QEvent::Type type)
{
    switch (type) {
    case QEvent::MouseButtonPress:
        return "press";
    case QEvent::MouseButtonDblClick:
        return "dclick";
    case QEvent::MouseButtonRelease:
        return "release";
    case QEvent::MouseMove:
        return "move";
    default:
        return "unknown";
    }
}

// 多层描边模拟辉光（SourceOver 下跨格式安全，优于 CompositionMode_Plus）
void drawGlowEllipse(QPainter *p, const QPointF &c, qreal r, const QColor &c1, qreal coreWidth, int layers)
{
    for (int i = layers; i >= 1; --i) {
        QColor col = c1;
        col.setAlphaF(qMin(1.0, c1.alphaF() * (0.9 / (i * 1.5))));
        QPen pen(col);
        pen.setWidthF(coreWidth * i * 2.1);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawEllipse(c, r, r);
    }
}
} // namespace

MouseTrackRecorder::MouseTrackRecorder()
{
    // 方案 A：参考图抠图素材（透明底卡通手套），构造时加载一次；
    // QImage 可在编码线程安全读取（QPixmap 不行）。
    m_gloveNormal = QImage(QStringLiteral(":/image/mousetrack/glove.png"))
                        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_glovePressed = m_gloveNormal.copy();
    for (int y = 0; y < m_glovePressed.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(m_glovePressed.scanLine(y));
        for (int x = 0; x < m_glovePressed.width(); ++x) {
            const QRgb c = line[x];
            line[x] = qRgba(int(qRed(c) * 0.9), int(qGreen(c) * 0.9), int(qBlue(c) * 0.9), qAlpha(c));
        }
    }
}

void MouseTrackRecorder::onMouseEvent(const QPointF &posInVideo, const QSize &videoSize, const QSize &frameSize,
                                      QEvent::Type type, Qt::MouseButton button)
{
    if (!isActive() || videoSize.isEmpty() || frameSize.isEmpty()) {
        return;
    }
    const QPointF pos = toFramePos(posInVideo, videoSize, frameSize);
    const qint64 ts = m_clock.elapsed();

    QMutexLocker locker(&m_mutex);
    m_cursor = pos;
    if (!m_cursorValid) {
        m_cursorSmoothed = pos;
        m_cursorValid = true;
    }

    switch (type) {
    case QEvent::MouseButtonPress:
        m_ripples.append({ts, pos, button == Qt::RightButton  ? RightButton
                                   : button == Qt::MiddleButton ? MiddleButton
                                                                : Press});
        if (button == Qt::LeftButton) {
            m_dragging = true;
            m_trail.append({ts, pos});
        }
        break;
    case QEvent::MouseButtonDblClick:
        m_ripples.append({ts, pos, DoubleClick});
        break;
    case QEvent::MouseButtonRelease:
        if (button == Qt::LeftButton) {
            m_dragging = false;
            m_trail.append({ts, pos});
        }
        break;
    case QEvent::MouseMove:
        if (m_dragging) {
            m_trail.append({ts, pos});
            // 尾迹点数上限：防止高频 move 撑爆缓冲（约 2 分钟拖拽）
            while (m_trail.size() > 2048) {
                m_trail.removeFirst();
            }
        }
        break;
    default:
        break;
    }

    if (m_logFile.isOpen()) {
        appendEventLog(QStringLiteral("{\"ts\":%1,\"type\":\"%2\",\"btn\":%3,\"x\":%4,\"y\":%5}")
                           .arg(ts)
                           .arg(eventTypeString(type))
                           .arg(int(button))
                           .arg(QString::number(pos.x(), 'f', 1))
                           .arg(QString::number(pos.y(), 'f', 1)));
    }
}

void MouseTrackRecorder::onWheelEvent(const QPointF &posInVideo, const QSize &videoSize, const QSize &frameSize,
                                      int angleDeltaY)
{
    if (!isActive() || videoSize.isEmpty() || frameSize.isEmpty()) {
        return;
    }
    const QPointF pos = toFramePos(posInVideo, videoSize, frameSize);
    const qint64 ts = m_clock.elapsed();

    QMutexLocker locker(&m_mutex);
    m_cursor = pos;
    if (!m_cursorValid) {
        m_cursorSmoothed = pos;
        m_cursorValid = true;
    }
    m_wheelDir = angleDeltaY > 0 ? 1 : -1;
    m_wheelTs = ts;

    if (m_logFile.isOpen()) {
        appendEventLog(QStringLiteral("{\"ts\":%1,\"type\":\"wheel\",\"dir\":%2,\"x\":%3,\"y\":%4}")
                           .arg(ts)
                           .arg(m_wheelDir)
                           .arg(QString::number(pos.x(), 'f', 1))
                           .arg(QString::number(pos.y(), 'f', 1)));
    }
}

void MouseTrackRecorder::startSession(const QString &eventsPath)
{
    QMutexLocker locker(&m_mutex);
    m_clock.start();
    m_cursorValid = false;
    m_dragging = false;
    m_ripples.clear();
    m_trail.clear();
    m_wheelDir = 0;
    m_wheelTs = 0;
    m_sessionActive = true;
    if (!eventsPath.isEmpty()) {
        m_logFile.setFileName(eventsPath);
        if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            m_logFile.write("[\n");
        }
    }
}

void MouseTrackRecorder::endSession(qint64 videoPtsOffsetMs)
{
    QMutexLocker locker(&m_mutex);
    m_sessionActive = false;
    if (m_logFile.isOpen()) {
        // 记录墙钟 → 视频时间轴的偏移，供落盘轨迹对齐视频使用
        if (videoPtsOffsetMs >= 0) {
            m_logFile.write(QStringLiteral("{\"_meta\":{\"videoPtsOffsetMs\":%1}}\n")
                                .arg(videoPtsOffsetMs)
                                .toUtf8());
        }
        m_logFile.write("\n]\n");
        m_logFile.close();
    }
}

bool MouseTrackRecorder::isActive() const
{
    return m_sessionActive;
}

void MouseTrackRecorder::paintAt(QImage &img, qint64 wallMs, double scale)
{
    if (img.isNull() || scale <= 0) {
        return;
    }

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    QVector<Ripple> ripples;
    QVector<Pt> trail;
    bool dragging = false;
    QPointF cursor;
    int wheelDir = 0;
    qint64 wheelTs = 0;
    {
        QMutexLocker locker(&m_mutex);
        // 清理过期对象（惰性回收，避免单独的定时器）。
        // wallMs 与事件 ts 同为墙钟基准，年龄计算即时正确。
        while (!m_ripples.isEmpty() && wallMs - m_ripples.first().ts > m_style.rippleMs) {
            m_ripples.removeFirst();
        }
        while (!m_trail.isEmpty() && wallMs - m_trail.first().ts > m_style.trailMs && !m_dragging) {
            m_trail.removeFirst();
        }
        // 光标平滑：绘制帧间向目标位置靠拢
        if (m_cursorValid) {
            m_cursorSmoothed += (m_cursor - m_cursorSmoothed) * 0.45;
        }
        cursor = m_cursorSmoothed;
        ripples = m_ripples;
        trail = m_trail;
        dragging = m_dragging;
        wheelDir = m_wheelDir;
        wheelTs = m_wheelTs;
    }

    const double unit = scale; // 设备像素 → 画面图像像素

    // 拖拽尾迹：辉光底衬 + 亮芯双层逐段渐隐
    const QColor trailCore = QColor(255, 240, 168); // 亮芯（向白混合的金色）
    if (!trail.isEmpty()) {
        const QColor &base = m_style.trailColor;
        for (int i = 1; i < trail.size(); ++i) {
            const double age = wallMs - trail.at(i).ts;
            const double a = qBound(0.0, 1.0 - age / m_style.trailMs, 1.0);
            if (a <= 0) {
                continue;
            }
            const QPointF p1 = trail.at(i - 1).pos * unit;
            const QPointF p2 = trail.at(i).pos * unit;
            QColor glow = base;
            glow.setAlphaF(0.30 * a);
            QPen gp(glow);
            gp.setWidthF(qMax(1.0, 9.0 * unit * (0.4 + 0.6 * a)));
            gp.setCapStyle(Qt::RoundCap);
            p.setPen(gp);
            p.drawLine(p1, p2);
            QColor core = trailCore;
            core.setAlphaF(0.95 * a);
            QPen cp(core);
            cp.setWidthF(qMax(0.8, 2.6 * unit * (0.5 + 0.5 * a)));
            cp.setCapStyle(Qt::RoundCap);
            p.setPen(cp);
            p.drawLine(p1, p2);
        }
        if (dragging && m_cursorValid) {
            const QPointF p1 = trail.last().pos * unit;
            const QPointF p2 = cursor * unit;
            QColor glow = m_style.trailColor;
            glow.setAlphaF(0.30);
            QPen gp(glow);
            gp.setWidthF(qMax(1.0, 9.0 * unit));
            gp.setCapStyle(Qt::RoundCap);
            p.setPen(gp);
            p.drawLine(p1, p2);
            QColor core = trailCore;
            core.setAlphaF(0.95);
            QPen cp(core);
            cp.setWidthF(qMax(0.8, 2.6 * unit));
            cp.setCapStyle(Qt::RoundCap);
            p.setPen(cp);
            p.drawLine(p1, p2);
        }
    }

    // 点击波纹：径向渐变光晕 + 彩色辉光环 + 白色亮环 + 按下闪光
    for (const Ripple &r : ripples) {
        const double t = qBound(0.0, double(wallMs - r.ts) / m_style.rippleMs, 1.0);
        const QColor &color = r.type == RightButton  ? m_style.rightColor
                              : r.type == MiddleButton ? m_style.middleColor
                                                       : m_style.pressColor;
        const double baseR = m_style.cursorRadius * unit;
        const int loops = r.type == DoubleClick ? 2 : 1;
        for (int loop = 0; loop < loops; ++loop) {
            const double tt = qBound(0.0, t - loop * 0.18, 1.0);
            if (tt <= 0) {
                continue;
            }
            const double ease = 1.0 - (1.0 - tt) * (1.0 - tt); // easeOutQuad
            const double radius = baseR * (0.55 + 2.1 * ease);
            const double alpha = qPow(1.0 - tt, 1.4);
            const QPointF c = r.pos * unit;

            // 光晕盘：颜色带在环上渐变淡出
            QRadialGradient halo(c, radius * 1.45);
            halo.setColorAt(0.0, QColor(color.red(), color.green(), color.blue(), 0));
            halo.setColorAt(0.55, QColor(color.red(), color.green(), color.blue(), 0));
            halo.setColorAt(0.74, QColor(color.red(), color.green(), color.blue(), int(150 * alpha)));
            halo.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
            p.setPen(Qt::NoPen);
            p.setBrush(halo);
            p.drawEllipse(c, radius * 1.45, radius * 1.45);

            // 彩色辉光环 + 白色亮芯环
            QColor ring = color;
            ring.setAlphaF(0.95 * alpha);
            drawGlowEllipse(&p, c, radius, ring, qMax(0.8, 2.2 * unit * alpha), 3);
            QPen white(QColor(255, 255, 255, qMin(255, int(235 * alpha))));
            white.setWidthF(qMax(0.8, 1.6 * unit));
            p.setPen(white);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, radius, radius);

            // 按下瞬间中心闪光
            const double flash = qMax(0.0, 1.0 - tt * 4.5);
            if (flash > 0) {
                QRadialGradient spark(c, baseR * 1.1);
                spark.setColorAt(0.0, QColor(255, 255, 255, int(200 * flash)));
                spark.setColorAt(0.5, QColor(color.red(), color.green(), color.blue(), int(120 * flash)));
                spark.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
                p.setBrush(spark);
                p.setPen(Qt::NoPen);
                p.drawEllipse(c, baseR * 1.1, baseR * 1.1);
            }
        }
    }

    // 滚动方向指示：辉光箭头 + 光晕
    if (wheelDir != 0 && wallMs - wheelTs <= m_style.wheelFlashMs) {
        const double a = qBound(0.0, 1.0 - double(wallMs - wheelTs) / m_style.wheelFlashMs, 1.0);
        const QColor &color = m_style.pressColor;
        const QPointF center(cursor.x() * unit, (cursor.y() - m_style.cursorRadius * 1.7) * unit);
        const double s = 7 * unit;

        QRadialGradient halo(center, s * 2.6);
        halo.setColorAt(0.0, QColor(color.red(), color.green(), color.blue(), int(120 * a)));
        halo.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(center, s * 2.6, s * 2.6);

        QPolygonF arrow;
        if (wheelDir > 0) {
            arrow << QPointF(center.x(), center.y() - s) << QPointF(center.x() - s, center.y() + s * 0.6)
                  << QPointF(center.x() + s, center.y() + s * 0.6);
        } else {
            arrow << QPointF(center.x(), center.y() + s) << QPointF(center.x() - s, center.y() - s * 0.6)
                  << QPointF(center.x() + s, center.y() - s * 0.6);
        }
        QColor glow = color;
        glow.setAlphaF(0.75 * a);
        QPen gp(glow);
        gp.setWidthF(qMax(1.5, 4.5 * unit));
        gp.setJoinStyle(Qt::RoundJoin);
        p.setPen(gp);
        QColor fill = color;
        fill.setAlphaF(0.9 * a);
        p.setBrush(fill);
        p.drawPolygon(arrow);
    }

    // 方案 A：卡通手套光标（参考图抠图，黑白原色，自带黑描边保证任意背景对比）
    // 默认态 = 原图 + 呼吸动效；按下态 = 下压 + 轻旋 + 变暗（与 HTML 预览一致）。
    if (m_cursorValid && !cursor.isNull() && !m_gloveNormal.isNull()) {
        const QPointF c = cursor * unit;
        const double pulse = 1.0 + 0.05 * qSin(wallMs * 2 * M_PI / 1400.0);
        const double side = m_style.cursorRadius * 3.2 * unit * (dragging ? 1.0 : pulse);
        const QImage &img = dragging ? m_glovePressed : m_gloveNormal;
        const double h = side * double(img.height()) / img.width();

        p.save();
        p.translate(c);
        if (dragging) {
            p.scale(0.88, 0.88);
            p.translate(0, 5.0 / 150.0 * side);
            p.rotate(14);
        }
        p.drawImage(QRectF(-side / 2, -h / 2, side, h), img);
        p.restore();
    }
}

QPointF MouseTrackRecorder::toFramePos(const QPointF &posInVideo, const QSize &videoSize, const QSize &frameSize) const
{
    // 与 inputconvertnormal 的换算一致：视频控件坐标 ×(frameSize/showSize)
    return QPointF(posInVideo.x() * frameSize.width() / videoSize.width(),
                   posInVideo.y() * frameSize.height() / videoSize.height());
}

void MouseTrackRecorder::appendEventLog(const QString &line)
{
    // JSON Lines；QFile 有内核缓冲，小行高频追加可接受
    m_logFile.write((line + ",\n").toUtf8());
}
