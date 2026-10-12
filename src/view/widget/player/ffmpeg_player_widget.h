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
3             2026-4-13      cjx            优化视频定位播放精度

*****************************************************************/

#ifdef CAN_USE_FFMPEG

#ifndef _FFMPEG_PLAYER_WIDGET_H
#define _FFMPEG_PLAYER_WIDGET_H

#include <QAudioFormat>
#include <QCloseEvent>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLabel>
#include <QMutex>
#include <QPair>
#include <QPointF>
#include <QQueue>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <QWaitCondition>
#include <QWidget>

#include <atomic>

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
            return m_videoClock;
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
                    return adjustedDuration - elapsed;
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

// ==================== YUV帧数据结构 ====================
struct YUVFrameData
{
    QByteArray yData;
    QByteArray uData;
    QByteArray vData;
    int yLinesize = 0;
    int uLinesize = 0;
    int vLinesize = 0;
    int width = 0;
    int height = 0;
    qint64 pts = 0;
    qint64 duration = 40000;

    bool isValid() const
    {
        return !yData.isEmpty() && !uData.isEmpty() && !vData.isEmpty() &&
                width > 0 && height > 0;
    }

    void clear()
    {
        yData.clear();
        uData.clear();
        vData.clear();
        yLinesize = uLinesize = vLinesize = 0;
        width = height = 0;
        pts = 0;
        duration = 40000;
    }
};

// ==================== 帧缓冲区 ====================
class OptionalFrameBuffer
{
public:
    struct VideoFrame
    {
        QImage image;
        YUVFrameData yuvData;
        qint64 pts;         // 时间戳（微秒）
        qint64 duration;    // 原始帧持续时间（微秒）
        bool isYUV;

        VideoFrame() : pts(0), duration(40000), isYUV(false) {}
        VideoFrame(const QImage &img, qint64 p, qint64 d = 40000)
            : image(img), pts(p), duration(d), isYUV(false) {}
        VideoFrame(const YUVFrameData &yuv, qint64 p, qint64 d = 40000)
            : yuvData(yuv), pts(p), duration(d), isYUV(true) {}

        bool isValid() const
        {
            if (isYUV)
                return yuvData.isValid() && pts >= 0;
            return !image.isNull() && pts >= 0;
        }
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
            m_condition.wait(&m_mutex, timeoutMs);
        return m_frames.isEmpty() ? VideoFrame() : m_frames.dequeue();
    }

    void smartClear(int keepCount = 1)
    {
        QMutexLocker lock(&m_mutex);
        while (m_frames.size() > keepCount)
            m_frames.dequeue();
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
    
    void start() { m_running = true; }

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
            int w = m_device->write(reinterpret_cast<const char *>(data + written), toWrite);
            if (w > 0)
                written += w;
            else
                return false;
        }
        return written == size;
    }

    void notifyBufferReady() { m_bufferAvailable.release(); }

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

// ==================== 时间轴归一化 ====================
/**
 * @brief 把源时间戳映射到连续递增的播放时间轴
 *
 * 部分 DVD/VOB 的时间戳不可直接使用，实测同一碟上同时出现两类异常：
 *   1) 中途归零重来（VTS_01_1：第一遍 0→1439.4s，第二遍 0→998.3s，实际内容 2437.7s）
 *   2) 33 位 PTS 回绕未归一（VTS_01_2：源时间戳出现一次 +2^33 的前向跳变）
 * 直接用原始时间戳会导致：播放进度突然跳回或暴增、总时长被算成
 * "末时间戳 − 首时间戳"（实测少算 2.4 倍甚至变成 26 小时）。
 *
 * 处理方式：不跟随源时间戳的绝对位置，而是按"相邻帧时间差"推进时间轴，
 * 跳变处（回退或超出单帧合理跨度）按正常步长继续，并记录一个锚点。
 * 锚点用于把归一化时间反算回源时间，seek 才能定位。
 */
struct TimestampNormalizer
{
    /** 单条流的状态 */
    struct Clock
    {
        bool anchored = false;               ///< 是否已锚定过起始位置
        qint64 lastRawUs = AV_NOPTS_VALUE;   ///< 上一个原始时间戳（微秒）
        qint64 lastOutUs = 0;                ///< 当前归一化时间（微秒）
        qint64 lastStepUs = 0;               ///< 上一个正常步长（异常时兜底复用）
        qint64 pendingOutUs = -1;            ///< >=0 时直接锚定到该位置（seek 后使用）
        qint64 pendingRawUs = 0;             ///< 与 pendingOutUs 对应的源时间戳
        qint64 seekTargetUs = -1;            ///< >=0 时用"最接近该目标的锚点"反算（落点未知的 seek）
    };

    /// 锚点：(归一化时刻, 对应源时间戳)
    QVector<QPair<qint64, qint64>> anchors;

    TimestampNormalizer() { reset(); }

    /** 复位到初始状态 */
    void reset();
    /** 归一化一个时间戳（AV_NOPTS_VALUE 表示无时间戳，沿用上一个值） */
    qint64 normalize(qint64 rawUs, Clock &clock);
    /** 归一化时间 -> 源时间（seek 用） */
    qint64 toSource(qint64 normalizedUs) const;
    /** 源时间 -> 归一化时间：源时间戳可能重复出现（多趟），取最接近 targetUs 的候选 */
    qint64 resolveNearest(qint64 rawUs, qint64 targetUs) const;
    /** 并入外部（后台扫描）得到的锚点表，补齐播放过程没经过的时间戳跳变点 */
    void mergeAnchors(const QVector<QPair<qint64, qint64>> &other);
    /**
     * @brief seek 之后按目标位置重建状态，使后续解码帧从目标位置继续
     * @param landingKnown true=确定落点就是 toSource(normalizedUs)（按时间戳 seek）
     *                     false=落点未知（按字节位置 seek），第一帧用最接近目标的锚点反算
     */
    void rebase(qint64 normalizedUs, Clock &c1, Clock &c2, bool landingKnown = true);

private:
    bool m_hasOrigin = false;    ///< 是否已确定整条时间轴的共同起点
    qint64 m_originRawUs = 0;    ///< 共同起点的源时间戳（保证音视频相对偏移不被破坏）
};

// ==================== 内容时长扫描 ====================
/**
 * @brief 后台扫描媒体的真实内容时长
 *
 * 容器里的 duration 元数据并不可靠：部分 DVD/VOB 的时间戳会在中途归零重来
 * （实测 VTS_01_1：第一遍 0→1439.4s，第二遍 0→998.3s，实际内容 2437.7s），
 * 此时容器 duration 只等于"最后一个时间戳 − 第一个"，比真实内容短 2.4 倍，
 * 进度条总长会显示成十几分钟。
 *
 * 这里按与播放一致的"时间差累加 + 归零自愈"规则扫一遍包，得到真实总时长。
 * 在独立线程执行、使用独立的 AVFormatContext，不阻塞播放也不干扰解码。
 */
class MediaDurationScanner : public QThread
{
    Q_OBJECT
public:
    explicit MediaDurationScanner(QObject *parent = nullptr);
    ~MediaDurationScanner() override;

    /** 开始扫描指定媒体（会先取消上一次未完成的扫描） */
    void startScan(const QString &url);
    /** 请求取消（异步） */
    void cancel();
    /**
     * @brief 本次扫描发现的"归一化时间 <-> 源时间戳"锚点表
     *
     * 记录源时间戳在哪些位置发生了跳变（中途归零 / 33 位回绕）。这是文件的固有结构，
     * 也是把归一化时间反算回源时间（以及反过来）的依据。扫描遍历整个文件，
     * 所以能拿到全部跳变点；播放端只靠"播放经过"来积累的话，用户直接拖到后半段就会缺信息。
     */
    QVector<QPair<qint64, qint64>> scannedAnchors() const;

signals:
    /** 扫描完成（durationMs <= 0 表示无法确定） */
    void durationScanned(const QString &url, qint64 durationMs);

protected:
    void run() override;

private:
    QString m_url;
    std::atomic<bool> m_cancel{false};
    mutable QMutex m_anchorMutex;
    QVector<QPair<qint64, qint64>> m_anchors;
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
    void setYUVModeEnabled(bool enabled) { m_useYUVMode = enabled; }

    // 截图
    bool takeScreenshot(const QString &filePath);

    /**
     * @brief 静态方法：将YUV数据转换为QImage
     * @param yuvData YUV帧数据
     * @return 转换后的RGB图像
     * 
     * 此方法为静态公开方法，可在没有解码器实例的情况下调用。
     * 用于在无法使用OpenGL时将YUV数据转换为RGB显示。
     */
    static QImage convertYUVToImageStatic(const YUVFrameData &yuvData);

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
    void frameReadyYUV(const YUVFrameData &yuvData);
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
    /** 自然播放到文件末尾（EOF）结束信号，携带结束播放的来源 */
    void mediaEnded(const QString &url);
    /** 视频画面几何信息：像素尺寸与像素宽高比（sarNum/sarDen <=0 表示未知） */
    void videoGeometryChanged(int width, int height, int sarNum, int sarDen);

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
    void processVideoFrameDirect(AVFrame *frame, qint64 pts);
    void processVideoFrameBuffered(AVFrame *frame, qint64 pts);

    YUVFrameData extractYUVData(AVFrame *frame);
    QImage convertFrameToImage(AVFrame *frame);

    void writeAudioData(const uint8_t *data, int size);
    void applyVolume(int16_t *samples, int count);
    void cleanupResources();
    void updatePerformanceStats();
    void reinitAudioOutput();

    bool is4KVideo() const;
    void performPreciseSeek(qint64 targetMs);

    // ==================== 时间戳归一化 ====================
    /**
     * @brief 把源时间戳映射到连续递增的播放时间轴
     * @param rawUs 源时间戳（微秒，AV_NOPTS_VALUE 表示无时间戳）
     * @param clock 该流的状态
     * @return 归一化后的时间戳（微秒）
     */
    qint64 normalizeTimestamp(qint64 rawUs, TimestampNormalizer::Clock &clock);
    /** 复位时间戳归一化状态（打开新媒体时调用） */
    void resetTimestampClocks();
    /** 归一化时间 -> 源时间（seek 时使用） */
    qint64 toSourceTimestamp(qint64 normalizedUs) const;
    /** seek 之后按目标位置重建归一化状态 */
    void rebaseTimestampClocks(qint64 normalizedUs, bool landingKnown = true);
    /**
     * @brief 按字节位置执行跳转（返回是否成功定位）
     *
     * 这两个 VOB 的源时间戳非单调（VTS_01_1 中途归零、VTS_01_2 有 33 位回绕），
     * 同一个时间戳在文件里出现多次，av_seek_frame(时间戳) 会定位错甚至直接跳到
     * 文件末尾读到 EOF。内容在文件里的存储顺序才是播放顺序，所以优先按字节位置跳。
     */
    bool seekByBytePosition(qint64 targetPts);
    /**
     * @brief 兜底跳转：从头顺序读到目标位置
     *
     * 按字节跳也失败时（例如时长未知导致估算偏差过大）使用。源时间戳非单调的文件里，
     * 顺序读是唯一一定正确的定位方式；只推进时间轴不解码，到目标前 2 秒再交回正常流程。
     */
    bool performSequentialSeek(qint64 targetMs);

    TimestampNormalizer m_tsNorm;              // 时间轴归一化状态（音视频共用同一套段划分）
    TimestampNormalizer::Clock m_videoClock;
    TimestampNormalizer::Clock m_audioClock;

    // 状态变量
    QMutex m_mutex;
    QWaitCondition m_pauseCondition;
    bool m_running = false;
    bool m_paused = false;
    bool m_muted = false;
    bool m_hasAudio = false;
    bool m_seekRequested = false;
    int m_volume = 100;
    int m_out_sample_rate = 44100;
    float m_playbackRate = 1.0f;
    bool m_needReinitAudio = false;
    int m_memoryLimitMB = 512;
    bool m_useYUVMode = false;          // 默认关闭，稳定后再开启
    // 精确seek相关
    qint64 m_seekPos = 0;
    qint64 m_seekTargetPts = -1;        // seek目标PTS（微秒）
    bool m_seekDecodingActive = false;  // 是否正在执行seek后的解码过滤
    // 本次 seek 是否已经用过"从头顺序读"兜底（只允许一次，避免反复重读整个文件）
    bool m_seekFallbackUsed = false;
    // 已经并入归一化状态的扫描锚点条数（守卫：同一份扫描结果只并一次）
    int m_mergedScanAnchorCount = -1;
    // 暂停时间追踪
    qint64 m_pauseStartTime = 0;        // 暂停开始时间（微秒）
    qint64 m_totalPausedTime = 0;       // 总暂停时长（微秒）
    bool m_wasPaused = false;           // 上一帧的暂停状态

    // 重连相关
    bool m_autoReconnect = false;
    int m_maxReconnectRetries = 3;
    int m_currentReconnectAttempt = 0;
    bool m_isReconnecting = false;

    // 时间信息
    qint64 m_durationMs = 0;
    // 后台扫描得到的真实内容时长（微秒）。容器 duration 对时间戳中途归零的 VOB 会少算一倍以上，
    // 按字节位置 seek 时要用它做估算；没扫出来之前不做字节 seek。
    qint64 m_scannedDurationMs = 0;
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
    // 后台时长扫描（修正容器 duration 元数据不准的问题）
    MediaDurationScanner *m_durationScanner = nullptr;

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
        AVRational sampleAspect = {1, 1};    // 像素宽高比（SAR），DVD/电视源常见非1:1
        int width = 0, height = 0;
        AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
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

    /**
     * @brief 释放播放资源（停止解码线程、关闭媒体文件与音频输出）
     *
     * 子控件不会收到 closeEvent，因此关闭承载它的窗口时必须显式调用本方法，
     * 否则解码线程、文件句柄与音频设备会一直保留。
     * 可重复调用。
     */
    void shutdown();

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

    // ==================== 画面显示控制 ====================
    // 说明：缩放模式 / 显示比例 / 倍率 / 平移的几何换算统一在本类完成，
    //       再下发给具体渲染组件（OpenGL 或 QLabel 软件渲染），保证两条渲染路径表现一致。

    /** 缩放倍率下限/上限 */
    static constexpr double kMinZoom = 0.1;
    static constexpr double kMaxZoom = 8.0;

    /** 设置画面缩放模式（适应窗口/铺满裁剪/拉伸/原始尺寸） */
    void setScaleMode(VideoScaleMode mode);
    VideoScaleMode scaleMode() const { return m_scaleMode_; }

    /** 设置显示比例覆盖（宽/高）；<=0 表示按视频自身像素宽高比(SAR)自动 */
    void setAspectOverride(double aspect);
    double aspectOverride() const { return m_aspectOverride_; }

    /** 设置缩放倍率（1.0 为按缩放模式算出的基准尺寸），自动限制在 [kMinZoom, kMaxZoom] */
    void setZoom(double zoom);
    double zoom() const { return m_zoom_; }

    /** 设置画面平移（窗口像素），会自动按当前缩放范围钳制 */
    void setPanOffset(const QPointF &offset);
    QPointF panOffset() const { return m_panOffset_; }

    /** 复位缩放倍率与平移（保留缩放模式与显示比例设置） */
    void resetView();

    /** 当前画面的实际显示比例（已结合 SAR 与显示比例覆盖），未知时返回 0 */
    double displayAspect() const;

    /** 视频区按当前显示比例推荐的尺寸（用于窗口自适应），未知时返回 QSize() */
    QSize preferredVideoSize() const;

signals:
    /** 缩放倍率变化 */
    void zoomChanged(double zoom);
    /** 画面平移变化（含被钳制后的值） */
    void panOffsetChanged(const QPointF &offset);

public slots:
    void play(const QString &url);
    void updateFrame(const QImage &image);
    void onBufferStatusChanged(int usagePercent, int droppedFrames);
    void onNetworkReconnecting(int attempt, int maxRetries);
    void onStatisticsUpdated(const FFmpegDecoderThread::Statistics &stats);
    // 视频画面几何（含像素宽高比）变化，更新显示比例与推荐尺寸
    void onVideoGeometryChanged(int width, int height, int sarNum, int sarDen);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    // 视频区内的滚轮缩放 / 拖动平移（事件先落到子显示组件上，用事件过滤器接管）
    bool eventFilter(QObject *watched, QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    // 按视频显示比例给出推荐尺寸，便于布局/窗口自适应
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void setupConnections();
    void updateDisplay();
    void initRenderWidget();
    void cleanupDecoder();

    /** 计算基础缩放（仅由缩放模式与显示比例决定，不含用户倍率） */
    void computeBaseScale(double &scaleX, double &scaleY) const;
    /** 按当前缩放模式/倍率/平移刷新渲染组件的画面映射 */
    void updateViewTransform();
    /** 把平移钳制到画面超出窗口的范围内 */
    QPointF clampedPan(double scaleX, double scaleY) const;
    /** 软件渲染路径：按与 OpenGL 一致的几何刷新 QLabel 画面 */
    void updateLabelFrame();
    /** 滚轮缩放（以光标位置为锚点） */
    void handleWheelZoom(QWheelEvent *event, const QPointF &posInWidget);
    /** 拖动平移的按下/移动/释放处理，返回是否已处理 */
    bool handleDragEvent(QEvent *event, const QPointF &posInWidget);

    QWidget *m_displayWidget_;               // 显示组件（QLabel或OpenGLWidget）
    QLabel *m_displayLabel_;                 // QLabel软件渲染组件（降级备用）
    PlayerWidgetBase *m_playerCore_;
    FFmpegDecoderThread *m_decoder_;

    QImage m_currentFrame;
    QMutex m_frameMutex;

    // 画面几何与显示比例（display aspect ratio，已结合像素宽高比）
    int m_videoWidth_ = 0;
    int m_videoHeight_ = 0;
    double m_displayAspect_ = 0.0;

    // 画面显示控制状态
    VideoScaleMode m_scaleMode_ = FitWindow;    // 缩放模式
    double m_aspectOverride_ = 0.0;             // 显示比例覆盖（<=0 表示自动按 SAR）
    double m_zoom_ = 1.0;                       // 缩放倍率
    QPointF m_panOffset_;                       // 画面平移（窗口像素）
    bool m_dragging_ = false;                   // 是否正在拖动画面
    QPointF m_dragStartPos_;                    // 拖动起点（窗口像素）
    QPointF m_dragStartPan_;                    // 拖动开始时的平移量


    bool m_useOpenGL;                        // 是否尝试使用OpenGL渲染
    bool m_openglAvailable;                  // OpenGL是否可用
    bool m_firstFrameReceived;               // 是否已收到第一帧
    bool m_isClosing;
    
#if OPENGL_AVAILABLE
    OpenGLVideoWidget *m_glWidget_;          // OpenGL硬件加速渲染组件
#endif
};

#endif // _FFMPEG_PLAYER_WIDGET_H
#endif // CAN_USE_FFMPEG