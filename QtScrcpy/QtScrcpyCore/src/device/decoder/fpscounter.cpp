#include <QDebug>
#include <QTimerEvent>

#include "fpscounter.h"

FpsCounter::FpsCounter(QObject *parent) : QObject(parent) {}

FpsCounter::~FpsCounter() {}

void FpsCounter::start()
{
    resetCounter();
    startCounterTimer();
}

void FpsCounter::stop()
{
    stopCounterTimer();
    resetCounter();
}

bool FpsCounter::isStarted()
{
    return m_counterTimer;
}

void FpsCounter::addRenderedFrame()
{
    m_rendered++;
}

void FpsCounter::addSkippedFrame()
{
    m_skipped++;
}

void FpsCounter::timerEvent(QTimerEvent *event)
{
    if (event && m_counterTimer == event->timerId()) {
        m_curRendered = m_rendered;
        m_curSkipped = m_skipped;
        resetCounter();
        emit updateFPS(m_curRendered, m_curSkipped);
        // 每秒一行的帧率诊断日志：FPS=被渲染帧数，Discard=被跳过帧数。
        // 仅在用户开启"显示 FPS"时输出，避免未开显示时控制台被刷屏
        if (m_logEnabled) {
            qInfo("FPS:%d Discard:%d", m_curRendered, m_skipped);
        }
    }
}

void FpsCounter::startCounterTimer()
{
    stopCounterTimer();
    m_counterTimer = startTimer(1000);
}

void FpsCounter::stopCounterTimer()
{
    if (m_counterTimer) {
        killTimer(m_counterTimer);
        m_counterTimer = 0;
    }
}

void FpsCounter::resetCounter()
{
    m_rendered = 0;
    m_skipped = 0;
}
