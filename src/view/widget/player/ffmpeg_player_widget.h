/*****************************************************************
File:        ffmpeg_player_widget.h
Version:     2.1
Author:
start date:
Description: 基于FFmpeg库实现的多线程音视频播放器组件（通用跨平台）
    主要功能：
        1. 支持本地文件（MP4/AVI/MKV等）和网络流（RTSP/HTTP）播放
        2. 完整的播放控制：播放、暂停、停止、跳转、音量调节、静音
        3. 支持倍速/慢速播放（0.5x ~ 2.0x）
        4. 基于音频主时钟的精确音视频同步机制
        5. 可选帧缓冲队列，支持智能丢帧与缓冲区动态调整
        6. 网络流自动重连（可配置重试次数与退避延迟）
        7. 音频设备自适应容错（自动匹配采样率）
        8. 实时性能统计（帧率、丢帧数、码率、音频欠载）
        9. 截图保存功能
        10. 支持OpenGL硬件加速渲染（通过OPENGL_ENABLE宏控制）
    技术特性：
        1. 解码线程与UI线程分离，避免界面卡顿
        2. 支持Qt5/Qt6双版本编译
        3. 音视频时钟同步算法（差值阈值：-50ms~100ms）
        4. 环形帧缓冲队列，线程安全
        5. 音频非阻塞写入，缓冲状态信号通知

Version history
[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]
1             2026-4-08      cjx            create
2             2026-4-10      cjx            添加OpenGL硬件加速支持
                                             优化4K视频解码性能

*****************************************************************/

#ifdef CAN_USE_FFMPEG

#ifndef _FFMPEG_PLAYER_WIDGET_H
#define _FFMPEG_PLAYER_WIDGET_H

#include <QAudioFormat>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLabel>
#include <QMutex>
#include <QQueue>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>
#include <QWidget>

#if QT_VERSION_MAJOR < 6
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#else
#include <QAudioDevice>
#include <QAudioSink>
#include <QMediaDevices>
#endif

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
}

#include "base_player_widget.h"

// ==================== OpenGL支持检测 ====================
// 通过OPENGL_ENABLE宏控制是否启用OpenGL硬件加速渲染
#ifdef OPENGL_ENABLE
    #include "view/widget/opengl/opengl_video_widget.h"
    #define OPENGL_AVAILABLE 1
#else
    #define OPENGL_AVAILABLE 0
#endif

// ==================== 音视频同步管理器 ====================
class AVSyncManager
{
public:
    enum SyncMode
    {
        SYNC_AUDIO_MASTER,   // 音频主时钟（默认）
        SYNC_VIDEO_MASTER,   // 视频主时钟
        SYNC_EXTERNAL_CLOCK  // 外部时钟
    };

    AVSyncManager() { reset(); }

    /** 更新音频时钟 */
    void updateAudioClock(qint64 pts)
    {
        QMutexLocker lock(&m_mutex);
        m_audioClock = pts;
        m_audioClockTime = av_gettime_relative();
        m_audioClockValid = true;
    }

    /** 更新视频时钟 */
    void updateVideoClock(qint64 pts)
    {
        QMutexLocker lock(&m_mutex);
        m_videoClock = pts;
    }

    /** 获取当前主时钟值（微秒） */
    qint64 getCurrentClock() const
    {
        QMutexLocker lock(&m_mutex);

        if (m_syncMode == SYNC_AUDIO_MASTER && m_audioClockValid)
        {
            // 音频时钟 = 上次PTS + 经过的时间
            qint64 elapsed = av_gettime_relative() - m_audioClockTime;
            return m_audioClock + elapsed;
        }

        // 视频主时钟模式
        if (m_videoClock > 0)
        {
            return m_videoClock;
        }

        return 0;
    }

    /**
     * 计算视频帧需要等待的时间（支持倍速）
     * @param videoPts 视频帧的PTS（微秒）
     * @param frameDuration 原始帧持续时间（微秒）
     * @param playbackRate 播放倍速（0.5 = 慢速，1.0 = 正常，2.0 = 倍速）
     * @return 需要等待的微秒数，返回0表示立即显示，返回-1表示应该丢弃
     */
    qint64 calculateWaitTime(qint64 videoPts, qint64 frameDuration, float playbackRate = 1.0f)
    {
        QMutexLocker lock(&m_mutex);

        // 根据倍速调整帧持续时间
        qint64 adjustedDuration = static_cast<qint64>(frameDuration / playbackRate);
        
        if (!m_audioClockValid)
        {
            if (m_lastFrameTime > 0)
            {
                qint64 elapsed = av_gettime_relative() - m_lastFrameTime;
                if (elapsed < adjustedDuration)
                {
                    return adjustedDuration - elapsed;
                }
            }
            return 0;
        }
        
        // 获取当前主时钟值
        qint64 elapsed = av_gettime_relative() - m_audioClockTime;
        qint64 currentClock = m_audioClock + elapsed;
        
        // 倍速播放时，需要调整PTS比较：视频PTS按倍速缩放后与音频时钟比较
        // 实际上视频PTS本身不变，但音频时钟的推进速度已经受倍速影响（通过重采样）
        // 这里直接比较原始值，因为音频时钟的更新频率已经受倍速影响
        qint64 diff = videoPts - currentClock;
        
        const qint64 MAX_DIFF = 100000;      // 最大差值100ms
        const qint64 MIN_DIFF = -50000;      // 最小差值-50ms
        const qint64 SYNC_THRESHOLD = 10000; // 10ms内视为同步
        
        if (diff > MAX_DIFF)
        {
            // 视频超前太多，限制最大等待时间
            return MAX_DIFF;
        }
        
        if (diff < MIN_DIFF)
        {
            // 视频落后太多，丢弃这一帧
            return -1;
        }
        
        if (diff > SYNC_THRESHOLD)
        {
            // 视频超前，需要等待
            return diff;
        }
        
        // 在同步范围内，立即显示
        return 0;
    }

    /** 设置同步模式 */
    void setSyncMode(SyncMode mode)
    {
        QMutexLocker lock(&m_mutex);
        m_syncMode = mode;
    }

    /** 重置所有时钟 */
    void reset()
    {
        QMutexLocker lock(&m_mutex);
        m_audioClock = 0;
        m_videoClock = 0;
        m_audioClockTime = 0;
        m_audioClockValid = false;
        m_lastFrameTime = 0;
    }

    /** 记录帧已显示 */
    void frameDisplayed()
    {
        QMutexLocker lock(&m_mutex);
        m_lastFrameTime = av_gettime_relative();
    }

    /** 检查音频时钟是否有效 */
    bool isAudioClockValid() const
    {
        QMutexLocker lock(&m_mutex);
        return m_audioClockValid;
    }

private:
    mutable QMutex m_mutex;
    SyncMode m_syncMode = SYNC_AUDIO_MASTER;

    // 音频时钟
    qint64 m_audioClock = 0;
    qint64 m_audioClockTime = 0;
    bool m_audioClockValid = false;
    
    // 视频时钟
    qint64 m_videoClock = 0;
    
    // 帧间隔控制
    qint64 m_lastFrameTime = 0;
};

// ==================== 帧缓冲区 ====================
class OptionalFrameBuffer
{
public:
    struct VideoFrame
    {
        QImage image;
        qint64 pts;         // 时间戳（微秒）
        qint64 duration;    // 原始帧持续时间（微秒）

        VideoFrame() : pts(0), duration(40000) {}
        VideoFrame(const QImage &img, qint64 p, qint64 d = 40000)
            : image(img), pts(p), duration(d) {}

        bool isValid() const { return !image.isNull() && pts >= 0; }
    };

    explicit OptionalFrameBuffer(int maxSize = 15)
        : m_maxSize(maxSize), m_droppedFrames(0) {}

    bool push(const VideoFrame &frame)
    {
        QMutexLocker lock(&m_mutex);
        if (m_frames.size() >= m_maxSize && m_maxSize > 0)
        {
            m_frames.dequeue();
            m_droppedFrames++;
        }
        m_frames.enqueue(frame);
        m_condition.wakeOne();
        return true;
    }

    VideoFrame pop(int timeoutMs = 10)
    {
        QMutexLocker lock(&m_mutex);
        if (m_frames.isEmpty() && timeoutMs > 0)
        {
            m_condition.wait(&m_mutex, timeoutMs);
        }
        return m_frames.isEmpty() ? VideoFrame() : m_frames.dequeue();
    }

    void smartClear(int keepCount = 1)
    {
        QMutexLocker lock(&m_mutex);
        while (m_frames.size() > keepCount)
        {
            m_frames.dequeue();
        }
    }

    void clear()
    {
        QMutexLocker lock(&m_mutex);
        m_frames.clear();
        m_droppedFrames = 0;
    }

    int size() const
    {
        QMutexLocker lock(&m_mutex);
        return m_frames.size();
    }

    bool isEnabled() const { return m_maxSize > 0; }
    int droppedFrames() const { return m_droppedFrames; }

    void setMaxSize(int size)
    {
        QMutexLocker lock(&m_mutex);
        m_maxSize = qMax(0, size);
        if (m_maxSize == 0) m_frames.clear();
    }

private:
    QQueue<VideoFrame> m_frames;
    mutable QMutex m_mutex;
    QWaitCondition m_condition;
    int m_maxSize;
    int m_droppedFrames;
};

// ==================== 音频写入辅助类 ====================
class AudioWriter
{
public:
    AudioWriter() : m_device(nullptr), m_audioOutput(nullptr), m_running(true) {}
    ~AudioWriter() { stop(); }

    void setDevice(QIODevice *device, QObject *audioOutput)
    {
        QMutexLocker lock(&m_mutex);
        m_device = device;
        m_audioOutput = audioOutput;
        m_bufferAvailable.release();
    }

    void stop()
    {
        QMutexLocker lock(&m_mutex);
        m_running = false;
        m_bufferAvailable.release();
        m_device = nullptr;
        m_audioOutput = nullptr;
    }

    bool write(const uint8_t *data, int size, int timeoutMs = 100)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_device || !m_audioOutput || !m_running) return false;

        int written = 0;
        while (written < size && m_running)
        {
            int freeBytes = getFreeBytes();
            if (freeBytes <= 0)
            {
                lock.unlock();
                bool signaled = m_bufferAvailable.tryAcquire(1, timeoutMs);
                lock.relock();
                if (!signaled && !m_running) return false;
                continue;
            }

            int toWrite = qMin(size - written, freeBytes);
            int w = m_device->write(reinterpret_cast<const char*>(data + written), toWrite);
            if (w > 0)
            {
                written += w;
            }
            else
            {
                return false;
            }
        }
        return written == size;
    }

    void notifyBufferReady() { m_bufferAvailable.release(); }
    void start() { m_running = true; }

private:
    int getFreeBytes()
    {
        if (!m_audioOutput) return 0;
#if QT_VERSION_MAJOR < 6
        QAudioOutput *output = static_cast<QAudioOutput*>(m_audioOutput);
        return output ? output->bytesFree() : 0;
#else
        QAudioSink *sink = static_cast<QAudioSink*>(m_audioOutput);
        return sink ? sink->bytesFree() : 0;
#endif
    }

    QMutex m_mutex;
    QSemaphore m_bufferAvailable{0};
    QIODevice *m_device;
    QObject *m_audioOutput;
    bool m_running;
};

// ==================== 解码线程 ====================
class FFmpegDecoderThread : public QThread
{
    Q_OBJECT
public:
    explicit FFmpegDecoderThread(QObject *parent = nullptr);
    ~FFmpegDecoderThread() override;

    // 媒体控制
    bool openMedia(const QString &url);
    void close(bool waitForFinished = true);
    qint64 duration() const;
    bool isPaused() const;

    // 播放控制
    void setPaused();
    void seekTo(qint64 posMs);
    void setVolume(int volume);
    void setMute();
    /** 设置播放倍速（0.5x ~ 2.0x） */
    void setPlaybackRate(float rate);
    /** 获取当前播放倍速 */
    float playbackRate() const { return m_playbackRate; }

    // 配置
    void setFrameBufferEnabled(bool enabled);
    void setMaxBufferSize(int size);
    void setAutoReconnect(bool enabled, int maxRetries = 3);
    void setMemoryLimit(int limitMB);

    // 截图
    bool takeScreenshot(const QString &filePath);

    // 统计信息
    struct Statistics {
        int totalFramesDecoded = 0;
        int totalFramesDisplayed = 0;
        int droppedFrames = 0;
        int reconnectCount = 0;
        int audioUnderrunCount = 0;
        qint64 currentBitrate = 0;
        float currentPlaybackRate = 1.0f;
        float currentFps = 0.0f;
        int memoryUsageMB = 0;
    };
    Statistics getStatistics() const;

signals:
    /** 视频帧就绪信号 */
    void frameReady(const QImage &image);
    /** 播放位置变化信号（毫秒） */
    void positionChanged(qint64 pos);
    /** 媒体总时长变化信号（毫秒） */
    void durationChanged(qint64 duration);
    /** 播放状态变化信号 */
    void playStateChanged(PlayerWidgetBase::PlayState state);
    /** 音量变化信号（0-100） */
    void volumeChanged(int volume);
    /** 静音状态变化信号 */
    void muteStateChanged(bool isMute);
    /** 错误发生信号 */
    void errorOccurred(const QString &message);
    /** 缓冲区状态变化信号 */
    void bufferStatusChanged(int usagePercent, int droppedFrames);
    /** 网络重连中信号 */
    void networkReconnecting(int attempt, int maxRetries);
    /** 网络重连成功信号 */
    void networkReconnected();
    /** 统计信息更新信号 */
    void statisticsUpdated(const Statistics &stats);
    /** 播放倍速变化信号 */
    void playbackRateChanged(float rate);

protected:
    void run() override;

private:
    bool initVideo();
    bool initAudio();
    bool initAudioOutput();
    QAudioFormat createAudioFormat() const;
    bool reconnectMedia();
    void updateFrameRate();
    void adjustBufferForResolution();

    void decodeVideoPacket(AVPacket *pkt);
    void decodeAudioPacket(AVPacket *pkt);
    void processVideoFrameDirect(AVFrame *frame);
    void processVideoFrameBuffered(AVFrame *frame);
    QImage convertFrameToImage(AVFrame *frame);

    void writeAudioData(const uint8_t *data, int size);
    void applyVolume(int16_t *samples, int count);
    void cleanupResources();
    void updatePerformanceStats();
    /** 重新初始化音频输出（倍速变化时调用） */
    void reinitAudioOutput();

    bool is4KVideo() const;

    // 状态变量
    QMutex m_mutex;
    QWaitCondition m_pauseCondition;
    bool m_running = false;
    bool m_paused = false;
    bool m_muted = false;
    bool m_hasAudio = false;
    bool m_seekRequested = false;
    qint64 m_seekPos = 0;
    int m_volume = 100;
    int m_out_sample_rate = 44100;
    float m_playbackRate = 1.0f;
    bool m_needReinitAudio = false;
    int m_memoryLimitMB = 512;

    // 重连相关
    bool m_autoReconnect = false;
    int m_maxReconnectRetries = 3;
    int m_currentReconnectAttempt = 0;
    bool m_isReconnecting = false;

    // 时间信息
    qint64 m_lastDisplayTime = 0;
    qint64 m_frameInterval = 40000;
    qint64 m_lastVideoPts = 0;
    qint64 m_firstVideoPts = -1;
    qint64 m_startTime = 0;
    
    // 性能监控
    qint64 m_lastFpsCalcTime = 0;
    int m_framesSinceLastFpsCalc = 0;
    float m_currentFps = 0.0f;

    // 同步管理
    AVSyncManager m_syncManager;
    QString m_currentUrl;

    // 缓冲区
    OptionalFrameBuffer m_frameBuffer;
    bool m_useBuffer = true;

    // 内存复用
    uint8_t *m_rgbBuffer = nullptr;
    int m_rgbBufferSize = 0;

    // 音频写入优化
    AudioWriter m_audioWriter;
    QTimer *m_audioBufferTimer = nullptr;

    // 统计信息
    Statistics m_stats;
    qint64 m_lastStatTime = 0;
    qint64 m_lastBitrateCalcTime = 0;
    qint64 m_lastBitrateBytes = 0;

    // FFmpeg上下文
    AVFormatContext *m_formatCtx = nullptr;

    struct
    {
        int streamIndex = -1;
        AVCodecContext *codecCtx = nullptr;
        SwsContext *swsCtx = nullptr;
        AVRational frameRate = {0, 0};
        AVRational timeBase = {0, 0};
        int width = 0, height = 0;
    } m_video;

    struct
    {
        int streamIndex = -1;
        AVCodecContext *codecCtx = nullptr;
        SwrContext *swrCtx = nullptr;
        AVRational timeBase = {0, 0};
        int originalSampleRate = 0;      // 原始采样率
        int targetSampleRate = 44100;    // 重采样目标采样率（倍速调整后）
#if QT_VERSION_MAJOR < 6
        QAudioOutput *output = nullptr;
#else
        QAudioSink *output = nullptr;
#endif
        QIODevice *device = nullptr;
    } m_audio;
};

// ==================== 播放器Widget ====================
class FFmpegPlayer : public QWidget
{
    Q_OBJECT
public:
    explicit FFmpegPlayer(QWidget *parent = nullptr);
    ~FFmpegPlayer();

    PlayerWidgetBase *PlayerCore() const;
    void stop();

    // 配置方法
    void setFrameBufferEnabled(bool enabled);
    void setMaxBufferSize(int size);
    void setAutoReconnect(bool enabled, int maxRetries = 3);
    void setMemoryLimit(int limitMB);
    
    /**
     * @brief 设置是否使用OpenGL硬件加速渲染
     * @param enabled true启用OpenGL，false使用软件渲染
     * @note 仅在OPENGL_ENABLE宏定义且OpenGL可用时生效
     */
    void setUseOpenGL(bool enabled);
    
    /** 获取是否使用OpenGL渲染 */
    bool isUsingOpenGL() const { return m_useOpenGL && m_openglAvailable; }
    
    bool takeScreenshot(const QString &filePath);
    FFmpegDecoderThread::Statistics getStatistics() const;
    void setPlaybackRate(float rate);
    float playbackRate() const;

public slots:
    void play(const QString &url);
    void updateFrame(const QImage &image);
    void onBufferStatusChanged(int usagePercent, int droppedFrames);
    void onNetworkReconnecting(int attempt, int maxRetries);
    void onStatisticsUpdated(const FFmpegDecoderThread::Statistics &stats);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void setupConnections();
    void updateDisplay();
    void initRenderWidget();

    QWidget *m_displayWidget_;               // 显示组件（QLabel或OpenGLWidget）
    QLabel *m_displayLabel_;                 // QLabel软件渲染组件（降级备用）
    PlayerWidgetBase *m_playerCore_;
    FFmpegDecoderThread *m_decoder_;

    QImage m_currentFrame;
    QMutex m_frameMutex;
    
    bool m_useOpenGL;                        // 是否尝试使用OpenGL渲染
    bool m_openglAvailable;                  // OpenGL是否可用
    bool m_firstFrameReceived;               // 是否已收到第一帧
    
#if OPENGL_AVAILABLE
    OpenGLVideoWidget *m_glWidget_;          // OpenGL硬件加速渲染组件
#endif
};

#endif // _FFMPEG_PLAYER_WIDGET_H
#endif // CAN_USE_FFMPEG