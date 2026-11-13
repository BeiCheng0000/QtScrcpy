#ifndef VIDEO_BUFFER_H
#define VIDEO_BUFFER_H

#include <QMutex>
#include <QWaitCondition>
#include <QObject>

#include <functional>
#include "fpscounter.h"

// 前向声明
typedef struct AVFrame AVFrame;

class VideoBuffer : public QObject
{
    Q_OBJECT
public:
    VideoBuffer(QObject *parent = Q_NULLPTR);
    virtual ~VideoBuffer();

    bool init();
    void deInit();
    void lock();
    void unLock();
    void setRenderExpiredFrames(bool renderExpiredFrames);

    AVFrame *decodingFrame();
    // 将解码帧标记为可渲染
    // 该函数执行期间会锁定 m_mutex
    // 若上一帧已被消费则返回 true
    void offerDecodedFrame(bool &previousFrameSkipped);

    // 将渲染帧标记为已消费并返回它
    // 必须在持有 m_mutex 锁的情况下调用！！！
    // 调用方应在解锁 m_mutex 之前，将返回的帧渲染到某个纹理上
    const AVFrame *consumeRenderedFrame();

    void peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

    // 唤醒并避免任何阻塞调用
    void interrupt();

signals:
    void updateFPS(quint32 fps);

private:
    void swap();

private:
    AVFrame *m_decodingFrame = Q_NULLPTR;
    AVFrame *m_renderingframe = Q_NULLPTR;
    QMutex m_mutex;
    bool m_renderingFrameConsumed = true;
    FpsCounter m_fpsCounter;

    bool m_renderExpiredFrames = false;
    QWaitCondition m_renderingFrameConsumedCond;

    // 若不渲染过期帧，则不会用到 interrupted
    // 因为提供一帧永远不会阻塞
    bool m_interrupted = false;
};

#endif // VIDEO_BUFFER_H
