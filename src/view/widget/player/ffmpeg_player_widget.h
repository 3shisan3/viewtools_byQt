/*****************************************************************
File:        ffmpeg_player_widget.h
Version:     1.1
Author:
start date:
Description: 通过调用FFmpeg库实现的播放器组件
    主要功能：
        1. 播放本地视频文件
        2. 播放网络视频流
        3. 暂停、停止、快进、快退、音量调节等基本功能

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
#endif
#include <QElapsedTimer>
#include <QLabel>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <QWidget>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

#include "base_player_widget.h"

class FFmpegDecoderThread : public QThread
{
    Q_OBJECT
public:
    explicit FFmpegDecoderThread(QObject *parent = nullptr);
    ~FFmpegDecoderThread() override;

    bool openMedia(const QString &url);
    void close(bool waitForFinished = true);
    qint64 duration() const;
    bool isPaused() const;

signals:
    void frameReady(const QImage &image);
    void positionChanged(qint64 pos);
    void volumeChanged(int volume);
    void durationChanged(qint64 duration);
    void playStateChanged(PlayerWidgetBase::PlayState state);
    void muteStateChanged(bool isMute);
    void errorOccurred(const QString &message);

public slots:
    void setPaused();
    void seekTo(qint64 posMs);
    void setVolume(int volume);
    void setMute();

protected:
    void run() override;

private:
    // 音视频处理
    bool initVideo();
    bool initAudio();
    bool initAudioOutput();
    void decodeVideoPacket(AVPacket *pkt);
    void decodeAudioPacket(AVPacket *pkt);
    void displayVideoFrame(AVFrame *frame);
    QAudioFormat createAudioFormat() const;
    void applyVolume(uint8_t *data, int len);

    // 同步控制
    double getMasterClock();
    void updateVideoClock(double pts);
    void updateAudioClock(double pts);
    
    // 状态控制
    QMutex m_mutex;
    QWaitCondition m_pauseCondition;
    bool m_running = false;
    bool m_paused = false;
    bool m_muted = false;
    bool m_seekRequested = false;
    qint64 m_seekPos = 0;
    int m_volume = 100;
    int m_out_sample_rate = 44100;

    // 时间信息
    qint64 m_videoStartTime = 0;
    qint64 m_firstVideoPts = -1;
    double m_videoClock = 0;
    double m_audioClock = 0;
    qint64 m_lastFrameTime = 0;
    qint64 m_frameInterval = 40000;

    // FFmpeg相关
    AVFormatContext *m_formatCtx = nullptr;
    
    struct VideoStream
    {
        int streamIndex = -1;
        AVCodecContext *codecCtx = nullptr;
        SwsContext *swsCtx = nullptr;
    } m_video;

    struct AudioStream
    {
        int streamIndex = -1;
        AVCodecContext *codecCtx = nullptr;
        SwrContext *swrCtx = nullptr;
    #if QT_VERSION_MAJOR < 6
        QAudioOutput *output = nullptr;
    #else
        QAudioSink *output = nullptr;
    #endif
        QIODevice *device = nullptr;
        int bytesPerSample = 0;
    } m_audio;
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

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void setupConnections();

    QLabel *m_displayLabel_;
    PlayerWidgetBase *m_playerCore_;
    FFmpegDecoderThread *m_decoder_;
};

#endif // _FFMPEG_PLAYER_WIDGET_H

#endif // CAN_USE_FFMPEG