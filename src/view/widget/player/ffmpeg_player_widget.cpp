#ifdef CAN_USE_FFMPEG

#include "ffmpeg_player_widget.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QPainter>
#include <QResizeEvent>

#include <cmath>

// FFmpeg日志回调
static void ffmpegLogCallback(void *, int level, const char *fmt, va_list vl)
{
    if (level <= AV_LOG_WARNING)
    {
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), fmt, vl);
        qDebug() << "FFmpeg:" << buffer;
    }
}

// ==================== FFmpegDecoderThread 实现 ====================

FFmpegDecoderThread::FFmpegDecoderThread(QObject *parent)
    : QThread(parent)
    , m_frameBuffer(15)
    , m_lastVideoPts(0)
    , m_firstVideoPts(-1)
    , m_startTime(0)
{
    avformat_network_init();
    av_log_set_callback(ffmpegLogCallback);
    av_log_set_level(AV_LOG_WARNING);

    m_audioBufferTimer = new QTimer(this);
    m_audioBufferTimer->setInterval(50);
    connect(m_audioBufferTimer, &QTimer::timeout, this, [this]() {
        m_audioWriter.notifyBufferReady();
    });
}

FFmpegDecoderThread::~FFmpegDecoderThread()
{
    close(false); // 避免等待自身线程
    cleanupResources();

    if (m_rgbBuffer)
    {
        delete[] m_rgbBuffer;
        m_rgbBuffer = nullptr;
    }

    if (m_audioBufferTimer)
    {
        m_audioBufferTimer->stop();
        delete m_audioBufferTimer;
    }
}

void FFmpegDecoderThread::setFrameBufferEnabled(bool enabled)
{
    m_useBuffer = enabled;
    m_frameBuffer.setMaxSize(enabled ? 15 : 0);
}

void FFmpegDecoderThread::setMaxBufferSize(int size)
{
    m_frameBuffer.setMaxSize(size);
}

void FFmpegDecoderThread::setAutoReconnect(bool enabled, int maxRetries)
{
    m_autoReconnect = enabled;
    m_maxReconnectRetries = maxRetries;
    m_currentReconnectAttempt = 0;
}

FFmpegDecoderThread::Statistics FFmpegDecoderThread::getStatistics() const
{
    Statistics stats = m_stats;
    stats.currentPlaybackRate = m_playbackRate;
    return stats;
}

// ==================== 倍速播放核心实现 ====================

void FFmpegDecoderThread::setPlaybackRate(float rate)
{
    // 限制倍速范围 0.5x ~ 2.0x
    rate = qBound(0.5f, rate, 2.0f);
    
    QMutexLocker lock(&m_mutex);
    if (qFabs(m_playbackRate - rate) < 0.01f) return;  // 变化小于1%忽略
    
    m_playbackRate = rate;
    m_stats.currentPlaybackRate = rate;
    
    qDebug() << "Playback rate changed to:" << rate << "x";
    
    // 如果有音频，需要重新初始化音频输出以改变采样率
    if (m_hasAudio && m_audio.output)
    {
        m_needReinitAudio = true;
    }
    
    emit playbackRateChanged(rate);
}

void FFmpegDecoderThread::reinitAudioOutput()
{
    if (!m_hasAudio || !m_audio.output) return;
    
    qDebug() << "Reinitializing audio output for playback rate:" << m_playbackRate;
    
    // 停止当前音频输出
    m_audioWriter.stop();
    if (m_audio.output)
    {
        m_audio.output->stop();
    }
    
    // 计算新的目标采样率：原始采样率 * 倍速
    // 例如：原始44100Hz，2倍速 -> 88200Hz，0.5倍速 -> 22050Hz
    int newSampleRate = static_cast<int>(m_audio.originalSampleRate * m_playbackRate);
    // 限制采样率范围（常见音频设备支持范围）
    newSampleRate = qBound(8000, newSampleRate, 192000);
    
    if (newSampleRate == m_out_sample_rate && m_audio.targetSampleRate == newSampleRate)
    {
        // 采样率没变，只需重启音频
        if (m_audio.output)
        {
            m_audio.device = m_audio.output->start();
            if (m_audio.device)
            {
                m_audioWriter.setDevice(m_audio.device, m_audio.output);
                m_audioWriter.start();
            }
        }
        return;
    }
    
    m_out_sample_rate = newSampleRate;
    m_audio.targetSampleRate = newSampleRate;
    
    // 重新创建音频格式
    QAudioFormat format = createAudioFormat();
    
#if QT_VERSION_MAJOR < 6
    QAudioDeviceInfo deviceInfo = QAudioDeviceInfo::defaultOutputDevice();
    if (deviceInfo.isNull()) return;
    
    if (!deviceInfo.isFormatSupported(format))
    {
        // 尝试查找支持的采样率
        int rates[] = {newSampleRate, 48000, 44100, 32000, 24000, 22050, 16000};
        bool found = false;
        for (int sr : rates)
        {
            format.setSampleRate(sr);
            if (deviceInfo.isFormatSupported(format))
            {
                m_out_sample_rate = sr;
                m_audio.targetSampleRate = sr;
                found = true;
                break;
            }
        }
        if (!found) format = deviceInfo.nearestFormat(format);
    }
    
    delete m_audio.output;
    m_audio.output = new QAudioOutput(deviceInfo, format, this);
#else
    QAudioDevice deviceInfo = QMediaDevices::defaultAudioOutput();
    if (deviceInfo.isNull()) return;
    
    if (!deviceInfo.isFormatSupported(format))
    {
        int rates[] = {newSampleRate, 48000, 44100, 32000, 24000, 22050, 16000};
        bool found = false;
        for (int sr : rates)
        {
            format.setSampleRate(sr);
            if (deviceInfo.isFormatSupported(format))
            {
                m_out_sample_rate = sr;
                m_audio.targetSampleRate = sr;
                found = true;
                break;
            }
        }
        if (!found) return;
    }
    
    delete m_audio.output;
    m_audio.output = new QAudioSink(deviceInfo, format, this);
#endif
    
    m_audio.device = m_audio.output->start();
    if (m_audio.device)
    {
        m_audioWriter.setDevice(m_audio.device, m_audio.output);
        m_audioWriter.start();
    }
    
    // 重新配置重采样器以适应新的采样率
    if (m_audio.swrCtx)
    {
        swr_free(&m_audio.swrCtx);
        m_audio.swrCtx = swr_alloc();
        if (m_audio.swrCtx)
        {
            av_opt_set_chlayout(m_audio.swrCtx, "in_chlayout", &m_audio.codecCtx->ch_layout, 0);
            av_opt_set_int(m_audio.swrCtx, "in_sample_rate", m_audio.codecCtx->sample_rate, 0);
            av_opt_set_sample_fmt(m_audio.swrCtx, "in_sample_fmt", m_audio.codecCtx->sample_fmt, 0);
            
            AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
            av_opt_set_chlayout(m_audio.swrCtx, "out_chlayout", &out_layout, 0);
            av_opt_set_int(m_audio.swrCtx, "out_sample_rate", m_out_sample_rate, 0);
            av_opt_set_sample_fmt(m_audio.swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
            
            swr_init(m_audio.swrCtx);
        }
    }
    
    qDebug() << "Audio reinitialized - Target sample rate:" << m_out_sample_rate 
             << "Playback rate:" << m_playbackRate;
}

bool FFmpegDecoderThread::openMedia(const QString &url)
{
    close();

    QMutexLocker lock(&m_mutex);
    m_currentUrl = url;
    m_syncManager.reset();
    m_frameBuffer.clear();
    m_isReconnecting = false;
    m_currentReconnectAttempt = 0;
    m_lastVideoPts = 0;
    m_firstVideoPts = -1;
    m_startTime = 0;
    m_playbackRate = 1.0f;      // 重置倍速
    m_needReinitAudio = false;

    m_stats = Statistics();
    m_stats.currentPlaybackRate = 1.0f;
    m_lastBitrateCalcTime = av_gettime_relative();
    m_lastBitrateBytes = 0;

    AVDictionary *options = nullptr;
    bool isNetwork = url.startsWith("rtsp://") || url.startsWith("http://");

    if (isNetwork)
    {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "5000000", 0);
        av_dict_set(&options, "max_delay", "500000", 0);
        av_dict_set(&options, "buffer_size", "1048576", 0);
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_at_eof", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "5000000", 0);
    }
    else
    {
        av_dict_set(&options, "probesize", "32768", 0);
        av_dict_set(&options, "analyzeduration", "200000", 0);
    }

    int ret = avformat_open_input(&m_formatCtx, url.toUtf8().constData(), nullptr, &options);
    if (options) av_dict_free(&options);

    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(tr("Failed to open: %1").arg(errbuf));
        return false;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0)
    {
        emit errorOccurred(tr("No stream info"));
        return false;
    }

    bool videoOk = initVideo();
    bool audioOk = initAudio();

    if (!videoOk && !audioOk)
    {
        emit errorOccurred(tr("No valid streams"));
        return false;
    }

    if (m_hasAudio)
    {
        m_syncManager.setSyncMode(AVSyncManager::SYNC_AUDIO_MASTER);
        qDebug() << "Using audio master clock for sync";
    }
    else
    {
        m_syncManager.setSyncMode(AVSyncManager::SYNC_VIDEO_MASTER);
        qDebug() << "No audio, using video master clock";
    }

    qint64 duration = (m_formatCtx->duration != AV_NOPTS_VALUE)
                          ? m_formatCtx->duration * 1000 / AV_TIME_BASE : 0;
    emit durationChanged(duration);

    if (m_audioBufferTimer) m_audioBufferTimer->start();

    m_running = true;
    emit playStateChanged(PlayerWidgetBase::PlayingState);
    start();

    qDebug() << "Media opened - Video:" << videoOk << "Audio:" << m_hasAudio
             << "Duration:" << duration << "ms";
    return true;
}

bool FFmpegDecoderThread::reconnectMedia()
{
    if (!m_autoReconnect || m_isReconnecting) return false;

    m_isReconnecting = true;
    m_currentReconnectAttempt++;

    emit networkReconnecting(m_currentReconnectAttempt, m_maxReconnectRetries);
    qDebug() << "Attempting reconnect" << m_currentReconnectAttempt << "of"
             << m_maxReconnectRetries;

    qint64 currentPos = 0;
    {
        QMutexLocker lock(&m_mutex);
        currentPos = m_seekPos;
    }

    close(false);

    int delayMs = qMin(1000 * (1 << (m_currentReconnectAttempt - 1)), 30000);
    QThread::msleep(delayMs);

    if (openMedia(m_currentUrl))
    {
        // 恢复倍速设置
        if (m_playbackRate != 1.0f)
        {
            setPlaybackRate(m_playbackRate);
        }
        
        if (currentPos > 0)
        {
            seekTo(currentPos);
        }
        m_isReconnecting = false;
        m_currentReconnectAttempt = 0;
        m_stats.reconnectCount++;
        emit networkReconnected();
        qDebug() << "Reconnect successful";
        return true;
    }

    m_isReconnecting = false;

    if (m_currentReconnectAttempt >= m_maxReconnectRetries)
    {
        emit errorOccurred(tr("Max reconnection attempts reached"));
        return false;
    }

    return false;
}

bool FFmpegDecoderThread::initVideo()
{
    m_video.streamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_video.streamIndex < 0) return false;

    AVCodecParameters *params = m_formatCtx->streams[m_video.streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(params->codec_id);
    if (!codec) return false;

    m_video.codecCtx = avcodec_alloc_context3(codec);
    if (!m_video.codecCtx) return false;

    if (avcodec_parameters_to_context(m_video.codecCtx, params) < 0)
    {
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    if (avcodec_open2(m_video.codecCtx, codec, nullptr) < 0)
    {
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    m_video.width = m_video.codecCtx->width;
    m_video.height = m_video.codecCtx->height;
    m_video.timeBase = m_formatCtx->streams[m_video.streamIndex]->time_base;

    updateFrameRate();

    m_video.swsCtx = sws_getContext(m_video.width, m_video.height,
        m_video.codecCtx->pix_fmt, m_video.width, m_video.height,
        AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!m_video.swsCtx)
    {
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    m_rgbBufferSize = m_video.width * m_video.height * 4;
    m_rgbBuffer = new uint8_t[m_rgbBufferSize];

    qDebug() << "Video:" << m_video.width << "x" << m_video.height
             << "fps:" << (m_video.frameRate.num ? (double)m_video.frameRate.num / m_video.frameRate.den : 25);
    return true;
}

void FFmpegDecoderThread::updateFrameRate()
{
    if (!m_formatCtx || m_video.streamIndex < 0) return;
    
    AVStream *stream = m_formatCtx->streams[m_video.streamIndex];
    
    // 方法1: 从流中获取帧率
    AVRational fps = av_guess_frame_rate(m_formatCtx, stream, nullptr);
    if (fps.num > 0 && fps.den > 0)
    {
        m_video.frameRate = fps;
        m_frameInterval = 1000000 * fps.den / fps.num;
        qDebug() << "Frame rate from stream:" << (double)fps.num / fps.den << "fps";
        return;
    }
    
    // 方法2: 从codec中获取
    if (m_video.codecCtx && m_video.codecCtx->framerate.num > 0)
    {
        m_video.frameRate = m_video.codecCtx->framerate;
        m_frameInterval = 1000000 * m_video.frameRate.den / m_video.frameRate.num;
        qDebug() << "Frame rate from codec:" << (double)m_video.frameRate.num / m_video.frameRate.den << "fps";
        return;
    }
    
    // 方法3: 从时间基推算
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
    {
        m_video.frameRate = stream->avg_frame_rate;
        m_frameInterval = 1000000 * m_video.frameRate.den / m_video.frameRate.num;
        qDebug() << "Frame rate from avg:" << (double)m_video.frameRate.num / m_video.frameRate.den << "fps";
        return;
    }
    
    // 默认值：25fps
    m_video.frameRate = {25, 1};
    m_frameInterval = 40000;
    qDebug() << "Using default frame rate: 25fps";
}

bool FFmpegDecoderThread::initAudio()
{
    m_audio.streamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_audio.streamIndex < 0)
    {
        qDebug() << "No audio stream found, audio disabled";
        m_hasAudio = false;
        return true;  // 音频不存在不代表失败
    }

    // 获取解码器参数
    AVCodecParameters *params = m_formatCtx->streams[m_audio.streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(params->codec_id);
    if (!codec)
    {
        m_hasAudio = false;
        return true;
    }

    // 初始化解码上下文
    m_audio.codecCtx = avcodec_alloc_context3(codec);
    if (!m_audio.codecCtx || avcodec_parameters_to_context(m_audio.codecCtx, params) < 0)
    {
        qWarning() << "Failed to allocate audio codec context";
        avcodec_free_context(&m_audio.codecCtx);
        m_hasAudio = false;
        return true;
    }

    // 打开解码器
    if (avcodec_open2(m_audio.codecCtx, codec, nullptr) < 0)
    {
        avcodec_free_context(&m_audio.codecCtx);
        m_hasAudio = false;
        return true;
    }

    // 保存原始采样率
    m_audio.originalSampleRate = m_audio.codecCtx->sample_rate;
    m_audio.timeBase = m_formatCtx->streams[m_audio.streamIndex]->time_base;
    
    // 根据当前倍速计算目标采样率
    m_out_sample_rate = static_cast<int>(m_audio.originalSampleRate * m_playbackRate);
    m_out_sample_rate = qBound(8000, m_out_sample_rate, 192000);
    m_audio.targetSampleRate = m_out_sample_rate;
    
    // 初始化重采样器
    m_audio.swrCtx = swr_alloc();
    if (!m_audio.swrCtx)
    {
        avcodec_free_context(&m_audio.codecCtx);
        m_hasAudio = false;
        return true;
    }

    av_opt_set_chlayout(m_audio.swrCtx, "in_chlayout", &m_audio.codecCtx->ch_layout, 0);
    av_opt_set_int(m_audio.swrCtx, "in_sample_rate", m_audio.codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "in_sample_fmt", m_audio.codecCtx->sample_fmt, 0);

    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout(m_audio.swrCtx, "out_chlayout", &out_layout, 0);
    av_opt_set_int(m_audio.swrCtx, "out_sample_rate", m_out_sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if (swr_init(m_audio.swrCtx) < 0)
    {
        swr_free(&m_audio.swrCtx);
        avcodec_free_context(&m_audio.codecCtx);
        m_hasAudio = false;
        return true;
    }

    m_hasAudio = initAudioOutput();

    if (!m_hasAudio)
    {
        swr_free(&m_audio.swrCtx);
        avcodec_free_context(&m_audio.codecCtx);
    }

    return true;
}

bool FFmpegDecoderThread::initAudioOutput()
{
    QAudioFormat format = createAudioFormat();

#if QT_VERSION_MAJOR < 6
    QAudioDeviceInfo deviceInfo = QAudioDeviceInfo::defaultOutputDevice();
    if (deviceInfo.isNull()) return false;

    if (!deviceInfo.isFormatSupported(format))
    {
        int rates[] = {m_out_sample_rate, 48000, 44100, 32000, 24000, 22050, 16000};
        bool found = false;
        for (int sr : rates)
        {
            format.setSampleRate(sr);
            if (deviceInfo.isFormatSupported(format))
            {
                m_out_sample_rate = sr;
                m_audio.targetSampleRate = sr;
                found = true;
                break;
            }
        }
        if (!found) format = deviceInfo.nearestFormat(format);
    }

    m_audio.output = new QAudioOutput(deviceInfo, format, this);
#else
    QAudioDevice deviceInfo = QMediaDevices::defaultAudioOutput();
    if (deviceInfo.isNull()) return false;

    if (!deviceInfo.isFormatSupported(format))
    {
        // 尝试常见采样率
        int rates[] = {m_out_sample_rate, 48000, 44100, 32000, 24000, 22050, 16000};
        bool found = false;
        for (int sr : rates)
        {
            format.setSampleRate(sr);
            if (deviceInfo.isFormatSupported(format))
            {
                m_out_sample_rate = sr;
                m_audio.targetSampleRate = sr;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    m_audio.output = new QAudioSink(deviceInfo, format, this);
#endif

    m_audio.device = m_audio.output->start();
    if (!m_audio.device)
    {
        delete m_audio.output;
        m_audio.output = nullptr;
        return false;
    }

    m_audioWriter.start();
    m_audioWriter.setDevice(m_audio.device, m_audio.output);

    return true;
}

QAudioFormat FFmpegDecoderThread::createAudioFormat() const
{
    QAudioFormat format;
#if QT_VERSION_MAJOR < 6
    format.setSampleRate(m_out_sample_rate);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
#else
    format.setSampleRate(m_out_sample_rate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
#endif
    return format;
}

void FFmpegDecoderThread::close(bool waitForFinished)
{
    if (!m_running) return;

    {
        QMutexLocker lock(&m_mutex);
        m_running = false;
        m_paused = false;
        m_pauseCondition.wakeAll();
    }

    if (m_audioBufferTimer) m_audioBufferTimer->stop();
    m_audioWriter.stop();

    if (waitForFinished && this != QThread::currentThread())
    {
        wait(1000);
    }

    cleanupResources();
    emit playStateChanged(PlayerWidgetBase::StoppedState);
}

void FFmpegDecoderThread::cleanupResources()
{
    if (m_audio.output)
    {
        m_audio.output->stop();
        m_audio.output->deleteLater();
        m_audio.output = nullptr;
        m_audio.device = nullptr;
    }

    if (m_video.swsCtx) sws_freeContext(m_video.swsCtx);
    if (m_video.codecCtx) avcodec_free_context(&m_video.codecCtx);
    if (m_audio.swrCtx) swr_free(&m_audio.swrCtx);
    if (m_audio.codecCtx) avcodec_free_context(&m_audio.codecCtx);
    if (m_formatCtx) avformat_close_input(&m_formatCtx);

    m_video.streamIndex = -1;
    m_audio.streamIndex = -1;
    m_hasAudio = false;
    m_video.frameRate = {0, 0};
    m_frameInterval = 40000;
}

void FFmpegDecoderThread::run()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        emit errorOccurred(tr("Failed to allocate packet"));
        return;
    }

    bool isNetwork = m_currentUrl.startsWith("rtsp://") || m_currentUrl.startsWith("http://");
    m_lastStatTime = av_gettime_relative();
    int consecutiveErrors = 0;
    
    // 用于无音频模式的时间基准
    qint64 systemStartTime = 0;
    qint64 firstPts = -1;

    while (m_running)
    {
        // 处理暂停
        {
            QMutexLocker lock(&m_mutex);
            while (m_paused && m_running && !m_seekRequested)
            {
                m_pauseCondition.wait(&m_mutex);
            }
        }
        
        // 处理倍速变化导致的音频重初始化
        if (m_needReinitAudio && m_hasAudio)
        {
            m_needReinitAudio = false;
            reinitAudioOutput();
        }

        // 处理跳转
        if (m_seekRequested)
        {
            QMutexLocker lock(&m_mutex);
            int64_t target = av_rescale(m_seekPos, AV_TIME_BASE, 1000);
            if (av_seek_frame(m_formatCtx, -1, target, AVSEEK_FLAG_BACKWARD) >= 0)
            {
                if (m_video.codecCtx) avcodec_flush_buffers(m_video.codecCtx);
                if (m_audio.codecCtx) avcodec_flush_buffers(m_audio.codecCtx);
                m_frameBuffer.smartClear(1);
                m_syncManager.reset();
                m_lastVideoPts = 0;
                firstPts = -1;
                systemStartTime = 0;
                emit positionChanged(m_seekPos);
            }
            m_seekRequested = false;
            continue;
        }

        // 读取数据包
        av_packet_unref(pkt);
        int ret = av_read_frame(m_formatCtx, pkt);

        if (ret < 0)
        {
            if (ret == AVERROR_EOF) break;
            consecutiveErrors++;
            if (isNetwork && m_autoReconnect && consecutiveErrors > 3)
            {
                qDebug() << "Network error detected, attempting reconnect...";
                if (reconnectMedia())
                {
                    consecutiveErrors = 0;
                    continue;
                }
                break;
            }
            int waitMs = qMin(5 * (1 << qMin(consecutiveErrors, 6)), 200);
            QThread::msleep(waitMs);
            continue;
        }

        consecutiveErrors = 0;

        // 计算实时码率
        qint64 now = av_gettime_relative();
        if (now - m_lastBitrateCalcTime >= 1000000)
        {
            qint64 bytesDiff = m_lastBitrateBytes;
            m_stats.currentBitrate = bytesDiff * 8 / ((now - m_lastBitrateCalcTime) / 1000000);
            m_lastBitrateCalcTime = now;
            m_lastBitrateBytes = 0;
        }
        m_lastBitrateBytes += pkt->size;

        // 分发数据包
        if (pkt->stream_index == m_video.streamIndex)
        {
            decodeVideoPacket(pkt);
        }
        else if (pkt->stream_index == m_audio.streamIndex && m_hasAudio)
        {
            decodeAudioPacket(pkt);
        }

        // 处理视频帧显示
        if (m_useBuffer)
        {
            OptionalFrameBuffer::VideoFrame frame = m_frameBuffer.pop(5);
            if (frame.isValid())
            {
                qint64 waitTime = 0;
                
                if (m_hasAudio && m_syncManager.isAudioClockValid())
                {
                    // 有音频且音频时钟有效：使用音频时钟同步，传入倍速参数
                    waitTime = m_syncManager.calculateWaitTime(frame.pts, frame.duration, m_playbackRate);
                }
                else if (m_hasAudio && !m_syncManager.isAudioClockValid())
                {
                    // 音频时钟尚未建立，等待一小段时间
                    waitTime = 10000; // 等待10ms
                }
                else
                {
                    // 无音频：使用系统时钟同步，根据倍速调整帧间隔
                    qint64 adjustedDuration = static_cast<qint64>(frame.duration / m_playbackRate);
                    if (firstPts < 0)
                    {
                        firstPts = frame.pts;
                        systemStartTime = av_gettime_relative();
                        waitTime = 0;
                    }
                    else
                    {
                        qint64 expectedTime = systemStartTime + 
                            static_cast<qint64>((frame.pts - firstPts) / m_playbackRate);
                        qint64 currentTime = av_gettime_relative();
                        waitTime = expectedTime - currentTime;
                        
                        if (waitTime > adjustedDuration * 2)
                            waitTime = adjustedDuration * 2;
                        if (waitTime < -adjustedDuration)
                            waitTime = -1;
                    }
                }
                
                if (waitTime == -1)
                {
                    m_stats.droppedFrames++;
                    continue;
                }
                
                if (waitTime > 0)
                {
                    // 限制最大等待时间，避免长时间阻塞
                    waitTime = qMin(waitTime, static_cast<qint64>(frame.duration / m_playbackRate) * 2);
                    if (waitTime < 10000)
                    {
                        QThread::usleep(static_cast<unsigned long>(waitTime));
                    }
                    else
                    {
                        QThread::msleep(waitTime / 1000);
                    }
                }
                
                emit frameReady(frame.image);
                emit positionChanged(frame.pts / 1000);
                m_stats.totalFramesDisplayed++;
                m_syncManager.frameDisplayed();
                m_lastDisplayTime = av_gettime_relative();
            }
        }

        // 定期更新统计
        now = av_gettime_relative();
        if (now - m_lastStatTime >= 1000000)
        {
            updatePerformanceStats();
            m_lastStatTime = now;
            m_stats.currentPlaybackRate = m_playbackRate;
            emit statisticsUpdated(m_stats);
        }

        // 背压控制
        if (m_useBuffer && m_frameBuffer.size() > 20)
        {
            QThread::msleep(2);
        }
    }

    av_packet_free(&pkt);
    qDebug() << "Decoder stopped - Decoded:" << m_stats.totalFramesDecoded
             << "Displayed:" << m_stats.totalFramesDisplayed
             << "Dropped:" << m_stats.droppedFrames;
}

void FFmpegDecoderThread::decodeVideoPacket(AVPacket *pkt)
{
    int ret = avcodec_send_packet(m_video.codecCtx, pkt);
    if (ret < 0) return;

    while (m_running)
    {
        AVFrame *frame = av_frame_alloc();
        if (!frame) break;

        ret = avcodec_receive_frame(m_video.codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            av_frame_free(&frame);
            break;
        }
        if (ret < 0)
        {
            av_frame_free(&frame);
            break;
        }

        m_stats.totalFramesDecoded++;

        if (m_useBuffer)
            processVideoFrameBuffered(frame);
        else
            processVideoFrameDirect(frame);

        av_frame_free(&frame);
    }
}

void FFmpegDecoderThread::processVideoFrameDirect(AVFrame *frame)
{
    qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
    AVRational timeBase = m_formatCtx->streams[m_video.streamIndex]->time_base;
    pts = av_rescale_q(pts, timeBase, {1, 1000000});
    if (pts < 0) pts = 0;

    // 精确计算帧持续时间
    qint64 frameDuration = m_frameInterval;
    if (m_lastVideoPts > 0 && pts > m_lastVideoPts)
    {
        qint64 diff = pts - m_lastVideoPts;
        if (diff > 0 && diff < 200000)
        {
            frameDuration = (frameDuration * 3 + diff) / 4;
        }
    }
    frameDuration = qBound(8333LL, frameDuration, 1000000LL);
    m_lastVideoPts = pts;

    qint64 waitTime = 0;
    
    if (m_hasAudio && m_syncManager.isAudioClockValid())
    {
        waitTime = m_syncManager.calculateWaitTime(pts, frameDuration, m_playbackRate);
    }
    else if (m_hasAudio && !m_syncManager.isAudioClockValid())
    {
        // 音频时钟尚未建立，按帧间隔显示（考虑倍速）
        qint64 adjustedDuration = static_cast<qint64>(frameDuration / m_playbackRate);
        if (m_lastDisplayTime > 0)
        {
            qint64 elapsed = av_gettime_relative() - m_lastDisplayTime;
            if (elapsed < adjustedDuration)
            {
                waitTime = adjustedDuration - elapsed;
            }
        }
    }
    else
    {
        // 无音频：使用帧间隔控制（考虑倍速）
        qint64 adjustedDuration = static_cast<qint64>(frameDuration / m_playbackRate);
        if (m_lastDisplayTime > 0)
        {
            qint64 elapsed = av_gettime_relative() - m_lastDisplayTime;
            if (elapsed < adjustedDuration)
            {
                waitTime = adjustedDuration - elapsed;
            }
        }
    }
    
    if (waitTime == -1)
    {
        m_stats.droppedFrames++;
        return;
    }
    
    if (waitTime > 0)
    {
        waitTime = qMin(waitTime, static_cast<qint64>(frameDuration / m_playbackRate) * 2);
        if (waitTime < 10000)
        {
            QThread::usleep(static_cast<unsigned long>(waitTime));
        }
        else
        {
            QThread::msleep(waitTime / 1000);
        }
    }

    QImage image = convertFrameToImage(frame);
    if (!image.isNull())
    {
        emit frameReady(image);
        emit positionChanged(pts / 1000);
        m_stats.totalFramesDisplayed++;
        m_syncManager.frameDisplayed();
    }

    m_lastDisplayTime = av_gettime_relative();
}

void FFmpegDecoderThread::processVideoFrameBuffered(AVFrame *frame)
{
    qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
    AVRational timeBase = m_formatCtx->streams[m_video.streamIndex]->time_base;
    pts = av_rescale_q(pts, timeBase, {1, 1000000});
    if (pts < 0) pts = 0;

    // 精确计算帧持续时间（使用相邻帧PTS差值）
    qint64 frameDuration = m_frameInterval;
    if (m_lastVideoPts > 0 && pts > m_lastVideoPts)
    {
        qint64 diff = pts - m_lastVideoPts;
        if (diff > 0 && diff < 200000)
        {
            frameDuration = (frameDuration * 3 + diff) / 4;
        }
    }
    frameDuration = qBound(8333LL, frameDuration, 1000000LL);
    m_lastVideoPts = pts;

    QImage image = convertFrameToImage(frame);
    if (!image.isNull())
    {
        m_frameBuffer.push(OptionalFrameBuffer::VideoFrame(image, pts, frameDuration));
        m_syncManager.updateVideoClock(pts);
    }
}

QImage FFmpegDecoderThread::convertFrameToImage(AVFrame *frame)
{
    if (!m_video.swsCtx || !m_rgbBuffer) return QImage();

    uint8_t *data[1] = {m_rgbBuffer};
    int linesize[1] = {m_video.width * 4};

    sws_scale(m_video.swsCtx, frame->data, frame->linesize, 0,
              frame->height, data, linesize);

    return QImage(m_rgbBuffer, m_video.width, m_video.height, QImage::Format_RGB32);
}

bool FFmpegDecoderThread::takeScreenshot(const QString &filePath)
{
    OptionalFrameBuffer::VideoFrame frame;
    if (m_useBuffer)
    {
        frame = m_frameBuffer.pop(100);
    }
    else
    {
        return false;
    }

    if (!frame.isValid()) return false;
    return frame.image.save(filePath);
}

void FFmpegDecoderThread::decodeAudioPacket(AVPacket *pkt)
{
    int ret = avcodec_send_packet(m_audio.codecCtx, pkt);
    if (ret < 0) return;

    while (m_running)
    {
        AVFrame *frame = av_frame_alloc();
        if (!frame) break;

        ret = avcodec_receive_frame(m_audio.codecCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            av_frame_free(&frame);
            break;
        }
        if (ret < 0)
        {
            av_frame_free(&frame);
            break;
        }

        // 计算时间戳并更新音频时钟
        qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
        pts = av_rescale_q(pts, m_audio.timeBase, {1, 1000000});
        m_syncManager.updateAudioClock(pts);

        // 计算输出样本数（考虑倍速影响）
        int out_samples = av_rescale_rnd(
            swr_get_delay(m_audio.swrCtx, frame->sample_rate) + frame->nb_samples,
            m_out_sample_rate, frame->sample_rate, AV_ROUND_UP);

        // 分配输出缓冲区
        uint8_t *output = nullptr;
        if (av_samples_alloc(&output, nullptr, 2, out_samples, AV_SAMPLE_FMT_S16, 0) < 0)
        {
            av_frame_free(&frame);
            break;
        }

        // 重采样
        int realSamples = swr_convert(m_audio.swrCtx, &output, out_samples,
                                      (const uint8_t **)frame->data, frame->nb_samples);

        if (realSamples > 0 && m_audio.device)
        {
            int dataSize = realSamples * 4; // 2通道 * 2字节
            if (m_volume != 100)
            {
                applyVolume(reinterpret_cast<int16_t *>(output), dataSize / 2);
            }
            if (m_muted)
            {
                memset(output, 0, dataSize);
            }
            writeAudioData(output, dataSize);
        }

        av_freep(&output);
        av_frame_free(&frame);
    }
}

void FFmpegDecoderThread::writeAudioData(const uint8_t *data, int size)
{
    if (!m_audioWriter.write(data, size, 100))
    {
        m_stats.audioUnderrunCount++;
    }
}

void FFmpegDecoderThread::applyVolume(int16_t *samples, int count)
{
    float factor = m_volume / 100.0f;
    for (int i = 0; i < count; ++i)
    {
        samples[i] = qBound(-32768, static_cast<int>(samples[i] * factor), 32767);
    }
}

void FFmpegDecoderThread::updatePerformanceStats()
{
    if (m_useBuffer)
    {
        int usage = static_cast<int>(m_frameBuffer.size() * 100.0f / 15.0f);
        emit bufferStatusChanged(usage, m_frameBuffer.droppedFrames());
    }
}

qint64 FFmpegDecoderThread::duration() const
{
    if (!m_formatCtx || m_formatCtx->duration == AV_NOPTS_VALUE) return 0;
    return m_formatCtx->duration * 1000 / AV_TIME_BASE;
}

bool FFmpegDecoderThread::isPaused() const { return m_paused; }

void FFmpegDecoderThread::setPaused()
{
    QMutexLocker lock(&m_mutex);
    m_paused = !m_paused;
    if (!m_paused) m_pauseCondition.wakeAll();
    emit playStateChanged(m_paused ? PlayerWidgetBase::PausedState : PlayerWidgetBase::PlayingState);
}

void FFmpegDecoderThread::seekTo(qint64 posMs)
{
    QMutexLocker lock(&m_mutex);
    m_seekPos = qBound(0LL, posMs, duration());
    m_seekRequested = true;
}

void FFmpegDecoderThread::setVolume(int volume)
{
    m_volume = qBound(0, volume, 100);
    emit volumeChanged(m_volume);
}

void FFmpegDecoderThread::setMute()
{
    QMutexLocker lock(&m_mutex);
    m_muted = !m_muted;
    emit muteStateChanged(m_muted);
}

// ==================== FFmpegPlayer 实现 ====================

FFmpegPlayer::FFmpegPlayer(QWidget *parent)
    : QWidget(parent)
    , m_displayLabel_(new QLabel(this))
    , m_playerCore_(new PlayerWidgetBase(this))
    , m_decoder_(new FFmpegDecoderThread(this))
{
    m_displayLabel_->setAlignment(Qt::AlignCenter);
    m_displayLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_displayLabel_->setStyleSheet("background-color: black;");
    setupConnections();
}

FFmpegPlayer::~FFmpegPlayer()
{
    if (m_decoder_ && m_decoder_->isRunning())
    {
        disconnect(m_decoder_, nullptr, this, nullptr);
        m_decoder_->close(false);
        m_decoder_->deleteLater();
    }
}

PlayerWidgetBase *FFmpegPlayer::PlayerCore() const { return m_playerCore_; }

void FFmpegPlayer::setFrameBufferEnabled(bool enabled)
{
    m_decoder_->setFrameBufferEnabled(enabled);
}

void FFmpegPlayer::setMaxBufferSize(int size)
{
    m_decoder_->setMaxBufferSize(size);
}

void FFmpegPlayer::setAutoReconnect(bool enabled, int maxRetries)
{
    m_decoder_->setAutoReconnect(enabled, maxRetries);
}

bool FFmpegPlayer::takeScreenshot(const QString &filePath)
{
    return m_decoder_->takeScreenshot(filePath);
}

FFmpegDecoderThread::Statistics FFmpegPlayer::getStatistics() const
{
    return m_decoder_->getStatistics();
}

void FFmpegPlayer::setPlaybackRate(float rate)
{
    if (m_decoder_)
    {
        m_decoder_->setPlaybackRate(rate);
    }
}

float FFmpegPlayer::playbackRate() const
{
    return m_decoder_ ? m_decoder_->playbackRate() : 1.0f;
}

void FFmpegPlayer::setupConnections()
{
    connect(m_decoder_, &FFmpegDecoderThread::frameReady, this, &FFmpegPlayer::updateFrame, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::positionChanged, m_playerCore_, &PlayerWidgetBase::currentPosition, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::durationChanged, m_playerCore_, &PlayerWidgetBase::currentDuration, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::playbackRateChanged, m_playerCore_, &PlayerWidgetBase::currentPlaybackRate, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::playStateChanged, m_playerCore_, &PlayerWidgetBase::playStateChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::volumeChanged, m_playerCore_, &PlayerWidgetBase::currentVolume, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::muteStateChanged, m_playerCore_, &PlayerWidgetBase::muteStateChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::errorOccurred, m_playerCore_, &PlayerWidgetBase::errorInfoShow, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::bufferStatusChanged, this, &FFmpegPlayer::onBufferStatusChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::networkReconnecting, this, &FFmpegPlayer::onNetworkReconnecting, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::statisticsUpdated, this, &FFmpegPlayer::onStatisticsUpdated, Qt::QueuedConnection);

    connect(m_playerCore_, &PlayerWidgetBase::setPlayerFile, this, &FFmpegPlayer::play, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::setPlayerUrl, this, &FFmpegPlayer::play, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::changePlayState, m_decoder_, &FFmpegDecoderThread::setPaused, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::changeMuteState, m_decoder_, &FFmpegDecoderThread::setMute, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::seekPlay, m_decoder_, &FFmpegDecoderThread::seekTo, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::setVolume, m_decoder_, &FFmpegDecoderThread::setVolume, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::setPlaybackRate, this, &FFmpegPlayer::setPlaybackRate, Qt::QueuedConnection);
}

void FFmpegPlayer::play(const QString &url)
{
    if (m_decoder_->isRunning()) m_decoder_->close();
    m_decoder_->openMedia(url);
}

void FFmpegPlayer::stop()
{
    if (m_decoder_)
    {
        m_decoder_->close(false);
        connect(m_decoder_, &QThread::finished, m_decoder_, &QObject::deleteLater, Qt::QueuedConnection);
    }
}

void FFmpegPlayer::updateFrame(const QImage &image)
{
    if (image.isNull()) return;

    {
        QMutexLocker lock(&m_frameMutex);
        m_currentFrame = image;
    }
    updateDisplay();
}

void FFmpegPlayer::updateDisplay()
{
    QMutexLocker lock(&m_frameMutex);
    if (m_currentFrame.isNull()) return;

    QPixmap pixmap = QPixmap::fromImage(m_currentFrame)
        .scaled(m_displayLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_displayLabel_->setPixmap(pixmap);
}

void FFmpegPlayer::onBufferStatusChanged(int usagePercent, int droppedFrames)
{
    if (droppedFrames > 0)
    {
        qDebug() << "Buffer:" << usagePercent << "% Dropped:" << droppedFrames;
    }
}

void FFmpegPlayer::onNetworkReconnecting(int attempt, int maxRetries)
{
    qDebug() << "Network reconnecting:" << attempt << "/" << maxRetries;
}

void FFmpegPlayer::onStatisticsUpdated(const FFmpegDecoderThread::Statistics &stats)
{
    Q_UNUSED(stats)
}

void FFmpegPlayer::resizeEvent(QResizeEvent *event)
{
    m_displayLabel_->resize(event->size());
    updateDisplay();
    QWidget::resizeEvent(event);
}

void FFmpegPlayer::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    bool hasVideo = false;
    {
        QMutexLocker lock(&m_frameMutex);
        hasVideo = !m_currentFrame.isNull();
    }

    if (!hasVideo)
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, "No Video");
    }
}

#endif // CAN_USE_FFMPEG