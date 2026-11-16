#ifndef FPSCOUNTER_H
#define FPSCOUNTER_H
#include <QObject>

class FpsCounter : public QObject
{
    Q_OBJECT
public:
    FpsCounter(QObject *parent = Q_NULLPTR);
    virtual ~FpsCounter();

    void start();
    void stop();
    bool isStarted();
    void addRenderedFrame();
    void addSkippedFrame();
    // 控制每秒 FPS/Discard 控制台诊断日志（跟随"显示 FPS"开关，默认关闭）
    void setLogEnabled(bool enabled) { m_logEnabled = enabled; }

signals:
    // fps = 每秒被渲染消费的帧数；skipped = 每秒被跳过（未及渲染即被
    // 新帧覆盖）的帧数，二者之和近似为设备送出的帧率
    void updateFPS(quint32 fps, quint32 skipped);

protected:
    virtual void timerEvent(QTimerEvent *event);

private:
    void startCounterTimer();
    void stopCounterTimer();
    void resetCounter();

private:
    qint32 m_counterTimer = 0;
    quint32 m_curRendered = 0;
    quint32 m_curSkipped = 0;

    quint32 m_rendered = 0;
    quint32 m_skipped = 0;
    bool m_logEnabled = false;
};

#endif // FPSCOUNTER_H
