/*****************************************************************
File:        ffmpeg_player_widget.h
Version:     1.2
Author:
start date:
Description: 通过调用FFmpeg库实现的播放器组件
    主要功能：
        1. 播放本地视频文件
        2. 播放网络视频流
        3. 暂停、停止、快进、快退、音量调节等基本功能
    优化特性：
        1. 改进的音视频同步机制
        2. 音频设备容错处理
        3. 智能帧缓冲管理
        4. 性能优化和错误恢复

Version history
[序号][修改日期][修改者][修改内容]

*****************************************************************/

#ifdef CAN_USE_FFMPEG

#ifndef _FFMPEG_PLAYER_WIDGET_H
#define _FFMPEG_PLAYER_WIDGET_H

#include <QAudioFormat>
#if QT_VERSION_MAJOR < 6
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#else
#include <QMediaDevices>
#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#endif
#include <QElapsedTimer>
#include <QLabel>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <QWidget>
#include <QQueue>
#include <QDateTime>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "base_player_widget.h"

// 内联定义 AVSyncManager 类
class AVSyncManager
{
public:
    enum SyncMode {
        SYNC_AUDIO_MASTER,   // 音频主时钟（默认）
        SYNC_VIDEO_MASTER,   // 视频主时钟
        SYNC_EXTERNAL_CLOCK  // 外部时钟
    };
    
    AVSyncManager() : m_syncMode(SYNC_AUDIO_MASTER) {}
    
    /**
     * @brief 更新音频时钟
     * @param pts 音频时间戳（微秒）
     */
    void updateAudioClock(qint64 pts) {
        QMutexLocker lock(&m_mutex);
        m_audioClock = pts;
        m_audioClockUpdated = QDateTime::currentMSecsSinceEpoch();
    }
    
    /**
     * @brief 更新视频时钟
     * @param pts 视频时间戳（微秒）
     */
    void updateVideoClock(qint64 pts) {
        QMutexLocker lock(&m_mutex);
        m_videoClock = pts;
    }
    
    /**
     * @brief 获取主时钟当前值
     * @return 主时钟时间戳（微秒）
     */
    qint64 getMasterClock() const {
        QMutexLocker lock(&m_mutex);
        if (m_syncMode == SYNC_AUDIO_MASTER && m_audioClock > 0) {
            if (m_audioClockUpdated > 0) {
                // 计算音频时钟的当前值（考虑流逝的时间）
                qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_audioClockUpdated;
                return m_audioClock + elapsed * 1000; // 转为微秒
            }
            return m_audioClock;
        }
        return m_videoClock;
    }
    
    /**
     * @brief 计算视频帧的显示延迟
     * @param videoPts 视频帧时间戳（微秒）
     * @return 延迟时间（微秒），-1表示需要丢弃帧
     */
    qint64 calculateFrameDelay(qint64 videoPts) {
        qint64 clock = getMasterClock();
        if (clock <= 0) {
            return 0; // 主时钟未就绪
        }
        
        qint64 diff = videoPts - clock;
        
        // 同步阈值配置
        const qint64 SYNC_THRESHOLD = 10000;    // 10ms，理想同步范围
        const qint64 SYNC_FRAMEDROP = 100000;   // 100ms，超过此值丢弃帧
        
        if (diff < -SYNC_FRAMEDROP) {
            // 视频落后超过100ms，丢弃此帧
            return -1;
        } else if (diff < -SYNC_THRESHOLD) {
            // 视频落后10-100ms，不延迟
            return 0;
        } else if (diff > SYNC_THRESHOLD) {
            // 视频超前超过10ms，需要延迟
            return diff;
        }
        // 在同步阈值内，正常显示
        return 0;
    }
    
    /**
     * @brief 设置同步模式
     * @param mode 同步模式
     */
    void setSyncMode(SyncMode mode) {
        QMutexLocker lock(&m_mutex);
        m_syncMode = mode;
    }
    
    /**
     * @brief 获取同步模式
     */
    SyncMode getSyncMode() const {
        QMutexLocker lock(&m_mutex);
        return m_syncMode;
    }
    
    /**
     * @brief 重置所有时钟
     */
    void reset() {
        QMutexLocker lock(&m_mutex);
        m_audioClock = 0;
        m_videoClock = 0;
        m_audioClockUpdated = 0;
    }
    
private:
    mutable QMutex m_mutex;
    SyncMode m_syncMode;
    qint64 m_audioClock = 0;      // 音频时钟（微秒）
    qint64 m_videoClock = 0;      // 视频时钟（微秒）
    qint64 m_audioClockUpdated = 0; // 音频时钟最后更新时间（毫秒）
};

// 内联定义 FrameBuffer 类
class FrameBuffer
{
public:
    struct VideoFrame {
        QImage image;           // 视频帧图像
        qint64 pts;            // 时间戳（微秒）
        qint64 duration;       // 帧显示时长（微秒）
        
        VideoFrame() : pts(0), duration(0) {}
        VideoFrame(const QImage &img, qint64 p, qint64 dur) 
            : image(img), pts(p), duration(dur) {}
        
        bool isValid() const { return !image.isNull() && pts >= 0; }
    };
    
    FrameBuffer(int maxSize = 30) : m_maxSize(maxSize) {}
    
    /**
     * @brief 向缓冲区添加帧
     * @param frame 视频帧
     * @return true 添加成功
     */
    bool push(const VideoFrame &frame) {
        QMutexLocker lock(&m_mutex);
        if (m_frames.size() >= m_maxSize) {
            // 缓冲区已满，丢弃最老的一帧
            m_frames.dequeue();
            m_droppedFrames++;
        }
        m_frames.enqueue(frame);
        m_condition.wakeOne();  // 唤醒等待的消费者
        return true;
    }
    
    /**
     * @brief 从缓冲区获取帧
     * @param timeoutMs 等待超时时间（毫秒）
     * @return 视频帧，如果超时返回无效帧
     */
    VideoFrame pop(int timeoutMs = 100) {
        QMutexLocker lock(&m_mutex);
        if (m_frames.isEmpty() && timeoutMs > 0) {
            m_condition.wait(&m_mutex, timeoutMs);
        }
        return m_frames.isEmpty() ? VideoFrame() : m_frames.dequeue();
    }
    
    /**
     * @brief 清空缓冲区
     */
    void clear() {
        QMutexLocker lock(&m_mutex);
        m_frames.clear();
        m_droppedFrames = 0;
    }
    
    /**
     * @brief 获取缓冲区大小
     */
    int size() const {
        QMutexLocker lock(&m_mutex);
        return m_frames.size();
    }
    
    /**
     * @brief 检查缓冲区是否为空
     */
    bool isEmpty() const {
        QMutexLocker lock(&m_mutex);
        return m_frames.isEmpty();
    }
    
    /**
     * @brief 获取丢弃的帧数
     */
    int droppedFrames() const {
        QMutexLocker lock(&m_mutex);
        return m_droppedFrames;
    }
    
    /**
     * @brief 设置缓冲区最大大小
     * @param size 最大帧数
     */
    void setMaxSize(int size) {
        QMutexLocker lock(&m_mutex);
        m_maxSize = qMax(1, size);
    }
    
    /**
     * @brief 获取缓冲区容量使用率
     * @return 0.0-1.0的使用率
     */
    float usage() const {
        QMutexLocker lock(&m_mutex);
        return m_maxSize > 0 ? static_cast<float>(m_frames.size()) / m_maxSize : 0.0f;
    }
    
private:
    QQueue<VideoFrame> m_frames;       // 帧队列
    mutable QMutex m_mutex;            // 互斥锁
    QWaitCondition m_condition;        // 条件变量
    int m_maxSize;                     // 最大缓冲区大小
    int m_droppedFrames = 0;           // 丢弃的帧数
};

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
    void setVolume(float volume);
    void setMute();

signals:
    // 视频相关信号
    void frameReady(const QImage &image);
    void positionChanged(qint64 pos);
    
    // 音频相关信号
    void volumeChanged(int volume);
    void muteStateChanged(bool isMute);
    
    // 状态相关信号
    void durationChanged(qint64 duration);
    void playStateChanged(PlayerWidgetBase::PlayState state);
    void errorOccurred(const QString &message);
    
    // 性能监控信号
    void bufferStatusChanged(int usagePercent, int droppedFrames);

protected:
    void run() override;

private:
    // 音视频初始化
    bool initVideo();
    bool initAudio();
    QAudioFormat createAudioFormat() const;
    
    // 解码处理
    void decodeVideoPacket(AVPacket *pkt);
    void decodeAudioPacket(AVPacket *pkt);
    
    // 帧处理
    void processVideoFrame(AVFrame *frame);
    QImage convertFrameToImage(AVFrame *frame);
    
    // 音视频同步
    qint64 getMasterClock() const;
    void updateVideoClock(qint64 pts);
    void updateAudioClock(qint64 pts);
    
    // 音频处理
    void applyVolume(uint8_t *data, int len);
    bool writeAudioData(const uint8_t *data, qint64 size);
    
    // 辅助函数
    void adjustVideoClock(qint64 adjustment);
    void updatePerformanceStats();
    void cleanupResources();

    // 同步管理
    AVSyncManager m_syncManager;
    
    // 状态控制
    QMutex m_mutex;
    QWaitCondition m_pauseCondition;
    bool m_running = false;
    bool m_paused = false;
    bool m_muted = false;
    bool m_hasAudio = false;          // 音频是否可用
    bool m_seekRequested = false;
    qint64 m_seekPos = 0;
    float m_volume = 1.0f;
    int m_out_sample_rate = 44100;
    
    // 时间信息
    QElapsedTimer m_clockTimer;
    qint64 m_videoStartTime = 0;
    qint64 m_firstVideoPts = -1;
    qint64 m_lastDisplayTime = 0;
    
    // 缓冲区管理
    FrameBuffer m_frameBuffer;
    int m_maxBufferSize = 30;         // 最大缓冲区帧数
    
    // 性能统计
    int m_totalFramesDecoded = 0;
    int m_totalFramesDisplayed = 0;
    int m_droppedFrames = 0;
    qint64 m_lastStatUpdate = 0;
    
    // FFmpeg相关成员
    AVFormatContext *m_formatCtx = nullptr;
    
    struct VideoContext
    {
        int streamIndex = -1;
        AVCodecContext *codecCtx = nullptr;
        SwsContext *swsCtx = nullptr;
        qint64 clock = 0;
        AVRational frameRate = {0, 0};
        int width = 0;
        int height = 0;
    } m_video;
    
    struct AudioContext
    {
        int streamIndex = -1;
        AVCodecContext *codecCtx = nullptr;
        SwrContext *swrCtx = nullptr;
        qint64 clock = 0;
    #if QT_VERSION_MAJOR < 6
        QAudioOutput *output = nullptr;
    #else
        QAudioSink *output = nullptr;
    #endif
        QIODevice *device = nullptr;
        bool deviceReady = false;
    } m_audio;
    
    QString m_currentUrl; // 存储当前URL
};

class FFmpegPlayer : public QWidget
{
    Q_OBJECT
public:
    explicit FFmpegPlayer(QWidget *parent = nullptr);
    ~FFmpegPlayer();

    PlayerWidgetBase *PlayerCore() const;
    void stop();

public slots:
    void play(const QString &url);
    void updateFrame(const QImage &image);
    
    // 性能监控槽
    void onBufferStatusChanged(int usagePercent, int droppedFrames);

protected:
    void resizeEvent(QResizeEvent *event) override;
    
    // 显示事件处理
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;

private:
    void setupConnections();
    void updateDisplayLabel();

    QLabel *m_displayLabel_;
    PlayerWidgetBase *m_playerCore_;
    FFmpegDecoderThread *m_decoder_;
    
    // 当前显示帧
    QImage m_currentFrame;
    mutable QMutex m_frameMutex;
};

#endif // _FFMPEG_PLAYER_WIDGET_H

#endif // CAN_USE_FFMPEG