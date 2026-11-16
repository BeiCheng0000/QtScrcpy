#include "videobuffer.h"
#include "avframeconvert.h"
extern "C"
{
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
}

VideoBuffer::VideoBuffer(QObject *parent) : QObject(parent) {
    connect(&m_fpsCounter, &FpsCounter::updateFPS, this, &VideoBuffer::updateFPS);
}

VideoBuffer::~VideoBuffer() {}

bool VideoBuffer::init()
{
    m_decodingFrame = av_frame_alloc();
    if (!m_decodingFrame) {
        goto error;
    }

    m_renderingframe = av_frame_alloc();
    if (!m_renderingframe) {
        goto error;
    }

    // 初始时尚未有渲染帧，因此视为该帧已被消费
    m_renderingFrameConsumed = true;

    // FPS 统计不在此启动：VideoBuffer 在 Device 构造时就已创建，
    // 提前启动会让尚未连接/连接失败的设备每秒刷出 FPS:0，既制造日志噪音，
    // 也让真正"连上但不出帧"的故障难以辨识。改为首帧到达时惰性启动。
    return true;

error:
    deInit();
    return false;
}

void VideoBuffer::deInit()
{
    if (m_decodingFrame) {
        av_frame_free(&m_decodingFrame);
        m_decodingFrame = Q_NULLPTR;
    }
    if (m_renderingframe) {
        av_frame_free(&m_renderingframe);
        m_renderingframe = Q_NULLPTR;
    }
    m_fpsCounter.stop();
}

void VideoBuffer::lock()
{
    m_mutex.lock();
}

void VideoBuffer::unLock()
{
    m_mutex.unlock();
}

void VideoBuffer::setRenderExpiredFrames(bool renderExpiredFrames)
{
    m_renderExpiredFrames = renderExpiredFrames;
}

AVFrame *VideoBuffer::decodingFrame()
{
    return m_decodingFrame;
}

void VideoBuffer::offerDecodedFrame(bool &previousFrameSkipped)
{
    m_mutex.lock();

    // 首帧到达：此时才启动 FPS 统计（对应一次真实的视频会话）。
    // 本函数运行在解码线程，而 FpsCounter 的线程亲和性在 GUI 线程，
    // 必须跨线程投递启动调用（QObject::startTimer 不允许跨线程）
    if (!m_fpsCounter.isStarted()) {
        QMetaObject::invokeMethod(&m_fpsCounter, [this]() { m_fpsCounter.start(); }, Qt::QueuedConnection);
    }

    if (m_renderExpiredFrames) {
        // if m_renderExpiredFrames is enable, then the decoder must wait for the current
        // 等待帧被消费
        while (!m_renderingFrameConsumed && !m_interrupted) {
            m_renderingFrameConsumedCond.wait(&m_mutex);
        }
    } else {
        if (m_fpsCounter.isStarted() && !m_renderingFrameConsumed) {
            m_fpsCounter.addSkippedFrame();
        }
    }

    swap();
    previousFrameSkipped = !m_renderingFrameConsumed;
    m_renderingFrameConsumed = false;
    m_mutex.unlock();
}

const AVFrame *VideoBuffer::consumeRenderedFrame()
{
    Q_ASSERT(!m_renderingFrameConsumed);
    m_renderingFrameConsumed = true;
    if (m_fpsCounter.isStarted()) {
        m_fpsCounter.addRenderedFrame();
    }
    if (m_renderExpiredFrames) {
        // if m_renderExpiredFrames is enable, then notify the decoder the current frame is
        // 已被消费，因此可以推送新的一帧
        m_renderingFrameConsumedCond.wakeOne();
    }
    return m_renderingframe;
}

void VideoBuffer::peekRenderedFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame)
{
    if (!onFrame) {
        return;
    }

    lock();
    auto frame = m_renderingframe;
    int width = frame->width;
    int height = frame->height;
    int linesize = frame->linesize[0];

    // 创建缓冲区
    uint8_t* rgbBuffer = new uint8_t[linesize * height * 4];
    AVFrame *rgbFrame = av_frame_alloc();
    if (!rgbFrame) {
        delete [] rgbBuffer;
        return;
    }

    // 将缓冲区绑定到 AVFrame
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuffer, AV_PIX_FMT_RGB32, width, height, 4);

    // 颜色空间转换
    AVFrameConvert convert;
    convert.setSrcFrameInfo(width, height, AV_PIX_FMT_YUV420P);
    convert.setDstFrameInfo(width, height, AV_PIX_FMT_RGB32);
    bool ret = false;
    ret = convert.init();
    if (!ret) {
        delete [] rgbBuffer;
        av_free(rgbFrame);
        return;
    }
    ret = convert.convert(frame, rgbFrame);
    if (!ret) {
        delete [] rgbBuffer;
        av_free(rgbFrame);
        return;
    }
    convert.deInit();
    av_free(rgbFrame);
    unLock();

    onFrame(width, height, rgbBuffer);
    delete [] rgbBuffer;
}

void VideoBuffer::interrupt()
{
    if (m_renderExpiredFrames) {
        m_mutex.lock();
        m_interrupted = true;
        m_mutex.unlock();
        // 唤醒阻塞等待
        m_renderingFrameConsumedCond.wakeOne();
    }
}

void VideoBuffer::swap()
{
    AVFrame *tmp = m_decodingFrame;
    m_decodingFrame = m_renderingframe;
    m_renderingframe = tmp;
}
