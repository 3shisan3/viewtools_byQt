#ifdef CAN_USE_FFMPEG

#include "ffmpeg_player_widget.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QHideEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QThread>
#include <QTimer>

#include <cmath>

// ==================== 4K视频优化配置常量 ====================
#define _4K_WIDTH_THRESHOLD   3840
#define _4K_HEIGHT_THRESHOLD  2160
#define _4K_BUFFER_SIZE       2          // 4K缓冲区大小（帧）
#define _1080P_BUFFER_SIZE    8          // 1080p缓冲区大小
#define _LOW_RES_BUFFER_SIZE  12         // 低分辨率缓冲区大小

// 4K优化：最大等待时间系数（降低以减少队列堆积）
#define _4K_WAIT_TIME_FACTOR  0.5f       // 4K视频等待时间为正常的一半

/**
 * @brief 检查是否为4K视频
 * @return true表示当前视频分辨率达到4K标准
 */
bool FFmpegDecoderThread::is4KVideo() const
{
    return (m_video.width >= _4K_WIDTH_THRESHOLD || m_video.height >= _4K_HEIGHT_THRESHOLD);
}

// ==================== FFmpeg日志回调 ====================
/**
 * @brief FFmpeg日志回调函数
 * @param level 日志级别
 * @param fmt 格式化字符串
 * @param vl 可变参数列表
 * @note 仅输出WARNING及以上级别的日志，减少控制台输出
 */
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
    // 初始化FFmpeg网络模块（支持RTSP/HTTP等网络流）
    avformat_network_init();

    // 设置FFmpeg日志回调
    av_log_set_callback(ffmpegLogCallback);
    av_log_set_level(AV_LOG_WARNING);

    // 创建音频缓冲区定时器，定期通知音频设备有数据可写
    m_audioBufferTimer = new QTimer(this);
    m_audioBufferTimer->setInterval(50); // 50ms间隔
    connect(m_audioBufferTimer, &QTimer::timeout, this, [this]() {
        m_audioWriter.notifyBufferReady();
    });

    qDebug() << "FFmpegDecoderThread initialized, YUV mode:" << m_useYUVMode;
}

FFmpegDecoderThread::~FFmpegDecoderThread()
{
    qDebug() << "FFmpegDecoderThread destructor";
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

void FFmpegDecoderThread::setMemoryLimit(int limitMB)
{
    m_memoryLimitMB = qMax(64, limitMB);
    qDebug() << "Memory limit set to" << m_memoryLimitMB << "MB";
}

FFmpegDecoderThread::Statistics FFmpegDecoderThread::getStatistics() const
{
    Statistics stats = m_stats;
    stats.currentPlaybackRate = m_playbackRate;
    stats.currentFps = m_currentFps;
    return stats;
}

// ==================== 倍速播放核心实现 ====================

void FFmpegDecoderThread::setPlaybackRate(float rate)
{
    // 限制倍速范围 0.5x ~ 2.0x
    rate = qBound(0.5f, rate, 2.0f);

    QMutexLocker lock(&m_mutex);
    if (qFabs(m_playbackRate - rate) < 0.01f)
        return; // 变化小于1%忽略

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
    if (!m_hasAudio || !m_audio.output)
        return;

    qDebug() << "Reinitializing audio output for playback rate:" << m_playbackRate;

    // 停止当前音频输出
    m_audioWriter.stop();
    if (m_audio.output)
    {
        m_audio.output->stop();
    }

    // 根据倍速计算新采样率，限制在合理范围
    int newSampleRate = static_cast<int>(m_audio.originalSampleRate * m_playbackRate);
    // 限制采样率范围（常见音频设备支持范围 8kHz ~ 48kHz）
    newSampleRate = qBound(8000, newSampleRate, 48000);

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
    // Qt5音频设备处理
    QAudioDeviceInfo deviceInfo = QAudioDeviceInfo::defaultOutputDevice();
    if (deviceInfo.isNull())
    {
        qWarning() << "No audio device available";
        return;
    }

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
                qDebug() << "Using alternate sample rate:" << sr;
                break;
            }
        }
        if (!found)
            format = deviceInfo.nearestFormat(format);
    }

    delete m_audio.output;
    m_audio.output = new QAudioOutput(deviceInfo, format, this);
#else
    // Qt6音频设备处理
    QAudioDevice deviceInfo = QMediaDevices::defaultAudioOutput();
    if (deviceInfo.isNull())
    {
        qWarning() << "No audio device available";
        return;
    }

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
                qDebug() << "Using alternate sample rate:" << sr;
                break;
            }
        }
        if (!found)
        {
            qWarning() << "No supported audio format found";
            return;
        }
    }

    delete m_audio.output;
    m_audio.output = new QAudioSink(deviceInfo, format, this);
#endif

    m_audio.device = m_audio.output->start();
    if (!m_audio.device)
    {
        qWarning() << "Failed to start audio output";
        delete m_audio.output;
        m_audio.output = nullptr;
        return;
    }

    m_audioWriter.setDevice(m_audio.device, m_audio.output);
    m_audioWriter.start();

    // 重新配置重采样器
    if (m_audio.swrCtx)
    {
        swr_free(&m_audio.swrCtx);
        m_audio.swrCtx = swr_alloc();
        if (m_audio.swrCtx)
        {
            // 设置输入参数
            av_opt_set_chlayout(m_audio.swrCtx, "in_chlayout", &m_audio.codecCtx->ch_layout, 0);
            av_opt_set_int(m_audio.swrCtx, "in_sample_rate", m_audio.codecCtx->sample_rate, 0);
            av_opt_set_sample_fmt(m_audio.swrCtx, "in_sample_fmt", m_audio.codecCtx->sample_fmt, 0);

            // 设置输出参数（立体声，16位整数）
            AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
            av_opt_set_chlayout(m_audio.swrCtx, "out_chlayout", &out_layout, 0);
            av_opt_set_int(m_audio.swrCtx, "out_sample_rate", m_out_sample_rate, 0);
            av_opt_set_sample_fmt(m_audio.swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
            av_opt_set_int(m_audio.swrCtx, "precision", 16, 0);

            if (swr_init(m_audio.swrCtx) < 0)
            {
                qWarning() << "Failed to reinitialize audio resampler";
            }
        }
    }

    qDebug() << "Audio reinitialized - Target sample rate:" << m_out_sample_rate
             << "Playback rate:" << m_playbackRate;
}

// ==================== 媒体打开与初始化 ====================

/**
 * @brief 打开媒体文件或网络流
 * @param url 文件路径或网络URL
 * @return true表示成功，false表示失败
 *
 * 支持的URL格式：
 * - 本地文件：直接路径
 * - RTSP流：rtsp://ip:port/path
 * - HTTP流：http://ip:port/path
 */
bool FFmpegDecoderThread::openMedia(const QString &url)
{
    if (m_running || isRunning())
    {
        qDebug() << "Stopping previous playback before opening new media...";
        close(true); // 等待线程完全退出
    }

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

    // 设置FFmpeg选项
    AVDictionary *options = nullptr;
    bool isNetwork = url.startsWith("rtsp://") || url.startsWith("http://");

    if (isNetwork)
    {
        // 网络流优化选项
        av_dict_set(&options, "rtsp_transport", "tcp", 0);          // RTSP使用TCP传输
        av_dict_set(&options, "stimeout", "5000000", 0);            // 超时5秒
        av_dict_set(&options, "max_delay", "500000", 0);            // 最大延迟500ms
        av_dict_set(&options, "buffer_size", "1048576", 0);         // 缓冲区1MB
        av_dict_set(&options, "reconnect", "1", 0);                 // 启用重连
        av_dict_set(&options, "reconnect_at_eof", "1", 0);          // EOF时重连
        av_dict_set(&options, "reconnect_streamed", "1", 0);        // 流式重连
        av_dict_set(&options, "reconnect_delay_max", "5000000", 0); // 最大重连延迟5秒
    }
    else
    {
        // 本地文件优化选项
        av_dict_set(&options, "probesize", "32768", 0);             // 探测大小32KB
        av_dict_set(&options, "analyzeduration", "200000", 0);      // 分析时长200ms
    }

    // 打开输入流
    int ret = avformat_open_input(&m_formatCtx, url.toUtf8().constData(), nullptr, &options);
    if (options)
        av_dict_free(&options);

    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(tr("Failed to open: %1").arg(errbuf));
        return false;
    }

    // 获取流信息
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0)
    {
        emit errorOccurred(tr("No stream info"));
        return false;
    }

    // 初始化视频和音频解码器
    bool videoOk = initVideo();
    bool audioOk = initAudio();

    if (!videoOk && !audioOk)
    {
        emit errorOccurred(tr("No valid streams"));
        return false;
    }

    // 设置同步模式
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

    // 获取总时长
    qint64 duration = (m_formatCtx->duration != AV_NOPTS_VALUE)
                          ? m_formatCtx->duration * 1000 / AV_TIME_BASE : 0;
    emit durationChanged(duration);

    // 启动音频缓冲区定时器
    if (m_audioBufferTimer)
        m_audioBufferTimer->start();

    // 启动解码线程
    m_running = true;
    emit playStateChanged(PlayerWidgetBase::PlayingState);
    start();

    qDebug() << "Media opened - Video:" << videoOk << "Audio:" << m_hasAudio
             << "Duration:" << duration << "ms";
    return true;
}

/**
 * @brief 网络重连
 * @return true表示重连成功
 * 
 * 使用指数退避策略：1s, 2s, 4s, 8s... 最大30秒
 */
bool FFmpegDecoderThread::reconnectMedia()
{
    if (!m_autoReconnect || m_isReconnecting)
        return false;

    m_isReconnecting = true;
    m_currentReconnectAttempt++;

    emit networkReconnecting(m_currentReconnectAttempt, m_maxReconnectRetries);
    qDebug() << "Attempting reconnect" << m_currentReconnectAttempt << "of"
             << m_maxReconnectRetries;

    // 保存当前播放位置
    qint64 currentPos = 0;
    {
        QMutexLocker lock(&m_mutex);
        currentPos = m_seekPos;
    }

    close(false);

    // 指数退避延迟
    int delayMs = qMin(1000 * (1 << (m_currentReconnectAttempt - 1)), 30000);
    QThread::msleep(delayMs);

    // 重新打开媒体
    if (openMedia(m_currentUrl))
    {
        // 恢复倍速设置
        if (m_playbackRate != 1.0f)
        {
            setPlaybackRate(m_playbackRate);
        }

        // 恢复播放位置
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

// 根据视频分辨率动态调整缓冲区大小
void FFmpegDecoderThread::adjustBufferForResolution()
{
    if (!m_useBuffer || m_video.width <= 0)
        return;

    // 根据分辨率计算百万像素数
    int megaPixels = (m_video.width * m_video.height) / (1024 * 1024);
    int bufferSize;

    // 4K视频使用极小缓冲区，减少内存压力
    if (m_video.width >= _4K_WIDTH_THRESHOLD || m_video.height >= _4K_HEIGHT_THRESHOLD)
    {
        bufferSize = _4K_BUFFER_SIZE;
        qDebug() << "4K video detected - using ultra-small buffer:" << bufferSize << "frames";

        // 4K视频强制启用快速解码选项
        if (m_video.codecCtx)
        {
            m_video.codecCtx->thread_count = qMax(4, QThread::idealThreadCount());
            m_video.codecCtx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
        }
    }
    else if (megaPixels >= 4)   // 2K (2560x1440 = 3.7MP)
    {
        bufferSize = _1080P_BUFFER_SIZE;
        qDebug() << "2K video detected - buffer size:" << bufferSize;
    }
    else if (megaPixels >= 2)   // 1080p
    {
        bufferSize = _1080P_BUFFER_SIZE;
        qDebug() << "1080p video detected - buffer size:" << bufferSize;
    }
    else                        // 低分辨率
    {
        bufferSize = _LOW_RES_BUFFER_SIZE;
        qDebug() << "Low resolution video - buffer size:" << bufferSize;
    }

    m_frameBuffer.setMaxSize(bufferSize);
}

YUVFrameData FFmpegDecoderThread::extractYUVData(AVFrame *frame)
{
    YUVFrameData data;

    if (!frame || frame->format != AV_PIX_FMT_YUV420P)
    {
        return data;
    }

    data.width = frame->width;
    data.height = frame->height;
    data.yLinesize = frame->linesize[0];
    data.uLinesize = frame->linesize[1];
    data.vLinesize = frame->linesize[2];

    int ySize = data.yLinesize * data.height;
    data.yData = QByteArray(reinterpret_cast<const char *>(frame->data[0]), ySize);

    int uvHeight = data.height / 2;
    int uSize = data.uLinesize * uvHeight;
    data.uData = QByteArray(reinterpret_cast<const char *>(frame->data[1]), uSize);

    int vSize = data.vLinesize * uvHeight;
    data.vData = QByteArray(reinterpret_cast<const char *>(frame->data[2]), vSize);

    return data;
}

// ==================== 视频初始化 ====================

bool FFmpegDecoderThread::initVideo()
{
    // 查找最佳视频流
    m_video.streamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_video.streamIndex < 0)
    {
        qWarning() << "No video stream found";
        return false;
    }

    // 获取解码器参数
    AVCodecParameters *params = m_formatCtx->streams[m_video.streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(params->codec_id);
    if (!codec)
    {
        qWarning() << "Video codec not found";
        return false;
    }

    // 分配解码器上下文
    m_video.codecCtx = avcodec_alloc_context3(codec);
    if (!m_video.codecCtx)
        return false;

    // 将参数填充到上下文
    if (avcodec_parameters_to_context(m_video.codecCtx, params) < 0)
    {
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    // 保存像素格式
    m_video.pixFmt = m_video.codecCtx->pix_fmt;

    // ========== 4K性能优化 ==========
    bool is4K = (m_video.codecCtx->width >= _4K_WIDTH_THRESHOLD ||
                 m_video.codecCtx->height >= _4K_HEIGHT_THRESHOLD);

    if (is4K)
    {
        // 4K视频：启用多线程解码
        int threadCount = qMax(4, QThread::idealThreadCount());
        m_video.codecCtx->thread_count = threadCount;
        m_video.codecCtx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
        qDebug() << "4K optimization enabled - thread count:" << threadCount;
    }
    else
    {
        // 普通视频：适度多线程
        m_video.codecCtx->thread_count = qMin(2, QThread::idealThreadCount());
        m_video.codecCtx->thread_type = FF_THREAD_SLICE;
    }

    // 打开解码器（不使用额外选项，避免兼容性问题）
    AVDictionary *opts = nullptr;
    if (avcodec_open2(m_video.codecCtx, codec, &opts) < 0)
    {
        av_dict_free(&opts);
        avcodec_free_context(&m_video.codecCtx);
        qWarning() << "Failed to open video codec";
        return false;
    }
    av_dict_free(&opts);

    // 获取视频尺寸和时间基
    m_video.width = m_video.codecCtx->width;
    m_video.height = m_video.codecCtx->height;
    m_video.timeBase = m_formatCtx->streams[m_video.streamIndex]->time_base;

    // 分辨率有效性检查
    if (m_video.width <= 0 || m_video.height <= 0)
    {
        qCritical() << "Invalid video resolution:" << m_video.width << "x" << m_video.height;
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    qDebug() << "Video resolution:" << m_video.width << "x" << m_video.height
             << "format:" << av_get_pix_fmt_name(m_video.pixFmt);

    // 动态调整缓冲区大小
    adjustBufferForResolution();
    updateFrameRate();

    // ========== 创建缩放上下文 ==========
    // 选择缩放算法：4K用快速算法，否则用双线性
    int swsFlags = is4K ? SWS_FAST_BILINEAR : SWS_BILINEAR;

    m_video.swsCtx = sws_getContext(m_video.width, m_video.height,
                                    m_video.codecCtx->pix_fmt, m_video.width, m_video.height,
                                    AV_PIX_FMT_RGB32, swsFlags, nullptr, nullptr, nullptr);

    if (!m_video.swsCtx)
    {
        qCritical() << "Failed to create sws context";
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    // ========== 分配RGB缓冲区 ==========
    m_rgbBufferSize = m_video.width * m_video.height * 4;
    m_rgbBuffer = new (std::nothrow) uint8_t[m_rgbBufferSize];
    if (!m_rgbBuffer)
    {
        qCritical() << "Failed to allocate RGB buffer";
        avcodec_free_context(&m_video.codecCtx);
        sws_freeContext(m_video.swsCtx);
        m_video.swsCtx = nullptr;
        return false;
    }

    qDebug() << "Video initialized successfully -" << m_video.width << "x" << m_video.height;
    return true;
}

void FFmpegDecoderThread::updateFrameRate()
{
    if (!m_formatCtx || m_video.streamIndex < 0)
        return;

    AVStream *stream = m_formatCtx->streams[m_video.streamIndex];

    // 方法1: 从流中获取帧率
    AVRational fps = av_guess_frame_rate(m_formatCtx, stream, nullptr);
    if (fps.num > 0 && fps.den > 0)
    {
        m_video.frameRate = fps;
        m_frameInterval = 1000000 * fps.den / fps.num;
        return;
    }

    // 方法2: 从codec中获取
    if (m_video.codecCtx && m_video.codecCtx->framerate.num > 0)
    {
        m_video.frameRate = m_video.codecCtx->framerate;
        m_frameInterval = 1000000 * m_video.frameRate.den / m_video.frameRate.num;
        return;
    }

    // 方法3: 从平均帧率推算
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
    {
        m_video.frameRate = stream->avg_frame_rate;
        m_frameInterval = 1000000 * m_video.frameRate.den / m_video.frameRate.num;
        return;
    }

    // 默认值：25fps
    m_video.frameRate = {25, 1};
    m_frameInterval = 40000; // 40ms = 25fps
    qDebug() << "Using default frame rate: 25fps";
}

bool FFmpegDecoderThread::initAudio()
{
    // 查找音频流（可选，没有音频不影响视频播放）
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
        qDebug() << "Audio codec not found, audio disabled";
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

    // ========== 音频采样率兼容性处理 ==========
    m_audio.originalSampleRate = m_audio.codecCtx->sample_rate;
    m_audio.timeBase = m_formatCtx->streams[m_audio.streamIndex]->time_base;
    
    // 限制输出采样率范围（通用兼容范围 8kHz ~ 48kHz）
    int minSampleRate = 8000;
    int maxSampleRate = 48000;

    // 计算目标采样率（考虑倍速）
    int targetRate = static_cast<int>(m_audio.originalSampleRate * m_playbackRate);
    targetRate = qBound(minSampleRate, targetRate, maxSampleRate);

    // 如果原始采样率已在合理范围内且倍速为1，保持原始采样率
    if (qFabs(m_playbackRate - 1.0f) < 0.01f &&
        m_audio.originalSampleRate >= minSampleRate &&
        m_audio.originalSampleRate <= maxSampleRate)
    {
        targetRate = m_audio.originalSampleRate;
    }

    m_out_sample_rate = targetRate;
    m_audio.targetSampleRate = targetRate;

    qDebug() << "Audio config - Original rate:" << m_audio.originalSampleRate
             << "Hz, Output rate:" << m_out_sample_rate << "Hz";

    // ========== 初始化重采样器 ==========
    m_audio.swrCtx = swr_alloc();
    if (!m_audio.swrCtx)
    {
        avcodec_free_context(&m_audio.codecCtx);
        m_hasAudio = false;
        return true;
    }

    // 设置输入参数
    av_opt_set_chlayout(m_audio.swrCtx, "in_chlayout", &m_audio.codecCtx->ch_layout, 0);
    av_opt_set_int(m_audio.swrCtx, "in_sample_rate", m_audio.codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "in_sample_fmt", m_audio.codecCtx->sample_fmt, 0);

    // 设置输出参数（立体声，16位整数）
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout(m_audio.swrCtx, "out_chlayout", &out_layout, 0);
    av_opt_set_int(m_audio.swrCtx, "out_sample_rate", m_out_sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    // 设置重采样精度（提高质量）
    av_opt_set_int(m_audio.swrCtx, "precision", 16, 0);

    if (swr_init(m_audio.swrCtx) < 0)
    {
        qWarning() << "Failed to initialize audio resampler";
        swr_free(&m_audio.swrCtx);
        avcodec_free_context(&m_audio.codecCtx);
        m_hasAudio = false;
        return true;
    }

    // 初始化音频输出
    m_hasAudio = initAudioOutput();

    if (!m_hasAudio)
    {
        swr_free(&m_audio.swrCtx);
        avcodec_free_context(&m_audio.codecCtx);
        qWarning() << "Audio output initialization failed, continuing without audio";
    }

    return true;
}

bool FFmpegDecoderThread::initAudioOutput()
{
    QAudioFormat format = createAudioFormat();

#if QT_VERSION_MAJOR < 6
    // Qt5音频设备处理
    QAudioDeviceInfo deviceInfo = QAudioDeviceInfo::defaultOutputDevice();
    if (deviceInfo.isNull())
        return false;

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
        if (!found)
            format = deviceInfo.nearestFormat(format);
    }

    m_audio.output = new QAudioOutput(deviceInfo, format, this);
#else
    // Qt6音频设备处理
    QAudioDevice deviceInfo = QMediaDevices::defaultAudioOutput();
    if (deviceInfo.isNull())
        return false;

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
        if (!found)
            return false;
    }

    m_audio.output = new QAudioSink(deviceInfo, format, this);
#endif

    // 启动音频输出
    m_audio.device = m_audio.output->start();
    if (!m_audio.device)
    {
        delete m_audio.output;
        m_audio.output = nullptr;
        return false;
    }

    // 设置音频写入器
    m_audioWriter.setDevice(m_audio.device, m_audio.output);
    m_audioWriter.start();

    return true;
}

/**
 * @brief 创建音频格式对象
 * @return QAudioFormat对象
 * 
 * 音频格式规范：
 * - 采样率：8kHz ~ 48kHz
 * - 通道数：2（立体声）
 * - 采样格式：16位有符号整数
 * - 字节序：小端
 */
QAudioFormat FFmpegDecoderThread::createAudioFormat() const
{
    QAudioFormat format;

    // 确保采样率在有效范围内，并使用标准采样率
    int safeRate = m_out_sample_rate;
    safeRate = qBound(8000, safeRate, 48000);

    // 检查是否为标准采样率，如果不是则使用最接近的
    const int standardRates[] = {8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000};
    bool isValidRate = false;
    for (int rate : standardRates)
    {
        if (safeRate == rate)
        {
            isValidRate = true;
            break;
        }
    }

    if (!isValidRate)
    {
        // 找到最接近的标准采样率
        int closestRate = 44100;
        int minDiff = abs(safeRate - 44100);
        for (int rate : standardRates)
        {
            int diff = abs(safeRate - rate);
            if (diff < minDiff)
            {
                minDiff = diff;
                closestRate = rate;
            }
        }
        safeRate = closestRate;
    }

#if QT_VERSION_MAJOR < 6
    format.setSampleRate(safeRate);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
#else
    format.setSampleRate(safeRate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
#endif

    return format;
}

// ==================== 资源清理 ====================

void FFmpegDecoderThread::close(bool waitForFinished)
{
    if (!m_running)
        return;

    qDebug() << "Closing decoder thread...";

    // 第一步：设置停止标志，通知线程退出
    {
        QMutexLocker lock(&m_mutex);
        m_running = false;
        m_paused = false;
        m_pauseCondition.wakeAll();
    }

    // 第二步：停止音频定时器
    if (m_audioBufferTimer)
    {
        m_audioBufferTimer->stop();
    }

    // 第三步：停止音频写入器
    m_audioWriter.stop();

    // 第四步：等待解码线程完全退出
    if (waitForFinished && isRunning())
    {
        qDebug() << "Waiting for decoder thread to finish...";
        if (!wait(5000)) // 增加到5秒
        {
            qWarning() << "Decoder thread did not finish in time, terminating...";
            terminate();
            if (!wait(2000))
            {
                qCritical() << "Failed to terminate decoder thread!";
            }
        }
        qDebug() << "Decoder thread finished";
    }

    // 第五步：线程完全退出后，再清理资源
    cleanupResources();

    emit playStateChanged(PlayerWidgetBase::StoppedState);

    qDebug() << "Decoder closed successfully";
}

void FFmpegDecoderThread::cleanupResources()
{
    qDebug() << "Cleaning up decoder resources...";

    // 确保线程已停止
    Q_ASSERT(!isRunning());

    // 清理音频输出（必须在清理其他资源前停止）
    if (m_audio.output)
    {
        m_audio.output->stop();
        m_audio.output->deleteLater();
        m_audio.output = nullptr;
        m_audio.device = nullptr;
    }

    // 清理视频转换上下文
    if (m_video.swsCtx)
    {
        sws_freeContext(m_video.swsCtx);
        m_video.swsCtx = nullptr;
    }

    // 清理视频解码器上下文
    if (m_video.codecCtx)
    {
        // 先刷新缓冲区
        avcodec_flush_buffers(m_video.codecCtx);
        avcodec_free_context(&m_video.codecCtx);
        m_video.codecCtx = nullptr;
    }

    // 清理音频重采样上下文
    if (m_audio.swrCtx)
    {
        swr_free(&m_audio.swrCtx);
        m_audio.swrCtx = nullptr;
    }

    // 清理音频解码器上下文
    if (m_audio.codecCtx)
    {
        avcodec_flush_buffers(m_audio.codecCtx);
        avcodec_free_context(&m_audio.codecCtx);
        m_audio.codecCtx = nullptr;
    }

    // 清理格式上下文
    if (m_formatCtx)
    {
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }

    // 清理RGB缓冲区
    if (m_rgbBuffer)
    {
        delete[] m_rgbBuffer;
        m_rgbBuffer = nullptr;
    }
    m_rgbBufferSize = 0;

    // 重置状态变量
    m_video.streamIndex = -1;
    m_audio.streamIndex = -1;
    m_hasAudio = false;
    m_video.frameRate = {0, 0};
    m_frameInterval = 40000;
    m_lastVideoPts = 0;
    m_firstVideoPts = -1;
    m_startTime = 0;

    // 清空帧缓冲区
    m_frameBuffer.clear();

    qDebug() << "Resources cleaned up";
}

// ==================== 解码线程主循环 ====================

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

    // 无音频模式的时间基准
    qint64 systemStartTime = 0;
    qint64 firstPts = -1;

    while (m_running)
    {
        // ========== 增加空指针检查 ==========
        if (!m_formatCtx)
        {
            qWarning() << "Format context is null, breaking decode loop";
            break;
        }
        if (!m_video.codecCtx && !m_audio.codecCtx)
        {
            qWarning() << "No valid codec contexts, breaking decode loop";
            break;
        }
        // ========== 检查结束 ==========

        {
            QMutexLocker lock(&m_mutex);
            while (m_paused && m_running && !m_seekRequested)
            {
                m_pauseCondition.wait(&m_mutex, 100);
            }
            if (!m_running)
                break;
        }

        // 处理倍速变化
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
            int ret = av_seek_frame(m_formatCtx, -1, target, AVSEEK_FLAG_BACKWARD);
            if (ret >= 0)
            {
                if (m_video.codecCtx)
                    avcodec_flush_buffers(m_video.codecCtx);
                if (m_audio.codecCtx)
                    avcodec_flush_buffers(m_audio.codecCtx);
                m_frameBuffer.smartClear(1);
                m_syncManager.reset();
                m_lastVideoPts = 0;
                firstPts = -1;
                systemStartTime = 0;
                emit positionChanged(m_seekPos);
            }
            m_seekRequested = false;
            m_seekPos = 0;
        }

        // 检查缓冲区是否过满
        bool is4K = (m_video.width >= _4K_WIDTH_THRESHOLD || m_video.height >= _4K_HEIGHT_THRESHOLD);
        int maxBufferSize = is4K ? 3 : 8;
        if (m_useBuffer && m_frameBuffer.size() >= maxBufferSize)
        {
            QThread::msleep(is4K ? 5 : 2);
            continue;
        }

        // 读取数据包
        av_packet_unref(pkt);
        int ret = av_read_frame(m_formatCtx, pkt);

        if (ret < 0)
        {
            if (ret == AVERROR_EOF)
            {
                // EOF 后等待一段时间再退出
                if (m_frameBuffer.size() > 0)
                {
                    QThread::msleep(10);
                    continue;
                }
                break;
            }

            consecutiveErrors++;

            // 网络流重连
            if (isNetwork && m_autoReconnect && consecutiveErrors > 5)
            {
                if (reconnectMedia())
                {
                    consecutiveErrors = 0;
                    continue;
                }
                break;
            }

            // 错误退避
            int waitMs = qMin(10 * consecutiveErrors, 100);
            QThread::msleep(waitMs);
            continue;
        }

        consecutiveErrors = 0;

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

        if (m_useBuffer)
        {
            OptionalFrameBuffer::VideoFrame frame = m_frameBuffer.pop(5);
            if (frame.isValid())
            {
                qint64 waitTime = 0;

                if (m_hasAudio && m_syncManager.isAudioClockValid())
                {
                    waitTime = m_syncManager.calculateWaitTime(frame.pts, frame.duration, m_playbackRate);
                }
                else if (m_hasAudio && !m_syncManager.isAudioClockValid())
                {
                    waitTime = 10000;
                }
                else
                {
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
                    qint64 maxWait;
                    if (is4K)
                    {
                        maxWait = static_cast<qint64>(frame.duration / m_playbackRate) * _4K_WAIT_TIME_FACTOR;
                        maxWait = qMax(maxWait, 10000LL);
                    }
                    else
                    {
                        maxWait = static_cast<qint64>(frame.duration / m_playbackRate) * 2;
                    }
                    waitTime = qMin(waitTime, maxWait);

                    if (waitTime < 10000)
                    {
                        QThread::usleep(static_cast<unsigned long>(waitTime));
                    }
                    else
                    {
                        QThread::msleep(waitTime / 1000);
                    }
                }

                if (!m_running)
                    break;

                // 正确发送帧数据
                // 根据帧类型发送对应的信号
                if (frame.isValid())
                {
                    if (frame.isYUV)
                    {
                        // 发送YUV帧数据（OpenGL渲染性能更好）
                        emit frameReadyYUV(frame.yuvData);
                    }
                    else
                    {
                        // 发送RGB图像数据
                        emit frameReady(frame.image);
                    }
                    emit positionChanged(frame.pts / 1000);
                    m_stats.totalFramesDisplayed++;
                    m_framesSinceLastFpsCalc++;
                    m_syncManager.frameDisplayed();
                }
                // ========== 修改部分结束 ==========

                m_lastDisplayTime = av_gettime_relative();
            }
        }

        now = av_gettime_relative();
        if (now - m_lastStatTime >= 1000000)
        {
            updatePerformanceStats();
            m_lastStatTime = now;
            m_stats.currentPlaybackRate = m_playbackRate;
            m_stats.currentFps = m_currentFps;
            m_stats.memoryUsageMB = m_rgbBufferSize / (1024 * 1024) +
                                    (m_frameBuffer.size() * m_rgbBufferSize) / (1024 * 1024);

            emit statisticsUpdated(m_stats);
        }
    }

    // 清空剩余的帧缓冲区
    while (m_frameBuffer.size() > 0 && m_running)
    {
        OptionalFrameBuffer::VideoFrame frame = m_frameBuffer.pop(10);
        if (frame.isValid())
        {
            if (frame.isYUV)
            {
                emit frameReadyYUV(frame.yuvData);
            }
            else
            {
                emit frameReady(frame.image);
            }
            emit positionChanged(frame.pts / 1000);
        }
    }

    av_packet_free(&pkt);
    qDebug() << "Decoder stopped - Decoded:" << m_stats.totalFramesDecoded
             << "Displayed:" << m_stats.totalFramesDisplayed
             << "Dropped:" << m_stats.droppedFrames;
}

// ==================== 视频解码与处理 ====================

void FFmpegDecoderThread::decodeVideoPacket(AVPacket *pkt)
{
    // 增加空指针检查
    if (!m_video.codecCtx)
    {
        qWarning() << "Video codec context is null";
        return;
    }
    if (!pkt)
    {
        qWarning() << "Packet is null";
        return;
    }

    int ret = avcodec_send_packet(m_video.codecCtx, pkt);
    if (ret < 0)
        return;

    while (m_running)
    {
        // 再次检查上下文是否有效
        if (!m_video.codecCtx)
            break;

        AVFrame *frame = av_frame_alloc();
        if (!frame)
            break;

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
    // 计算PTS（微秒）
    qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
    AVRational timeBase = m_formatCtx->streams[m_video.streamIndex]->time_base;
    pts = av_rescale_q(pts, timeBase, {1, 1000000});
    if (pts < 0)
        pts = 0;

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

    // 计算等待时间
    qint64 waitTime = 0;

    if (m_hasAudio && m_syncManager.isAudioClockValid())
    {
        waitTime = m_syncManager.calculateWaitTime(pts, frameDuration, m_playbackRate);
    }
    else if (m_hasAudio && !m_syncManager.isAudioClockValid())
    {
        // 音频时钟尚未建立，按帧间隔显示
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
        // 无音频：使用帧间隔控制
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

    // 转换并显示帧
    QImage image = convertFrameToImage(frame);
    if (!image.isNull())
    {
        emit frameReady(image);
        emit positionChanged(pts / 1000);
        m_stats.totalFramesDisplayed++;
        m_framesSinceLastFpsCalc++;
        m_syncManager.frameDisplayed();
    }

    m_lastDisplayTime = av_gettime_relative();
}

void FFmpegDecoderThread::processVideoFrameBuffered(AVFrame *frame)
{
    if (!frame)
        return;

    qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
    AVRational timeBase = m_formatCtx->streams[m_video.streamIndex]->time_base;
    pts = av_rescale_q(pts, timeBase, {1, 1000000});
    if (pts < 0)
        pts = 0;

    // 计算帧持续时间
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

    // 转换为RGB图像
    QImage image = convertFrameToImage(frame);
    if (!image.isNull())
    {
        // 创建VideoFrame并推入缓冲区
        OptionalFrameBuffer::VideoFrame videoFrame(image, pts, frameDuration);
        m_frameBuffer.push(videoFrame);
        m_syncManager.updateVideoClock(pts);
    }
    else
    {
        qWarning() << "Failed to convert frame to image";
    }
}

QImage FFmpegDecoderThread::convertFrameToImage(AVFrame *frame)
{
    if (!m_video.swsCtx || !m_rgbBuffer)
    {
        return QImage();
    }

    uint8_t *data[1] = {m_rgbBuffer};
    int linesize[1] = {m_video.width * 4};

    // YUV → RGB32 转换
    int ret = sws_scale(m_video.swsCtx, frame->data, frame->linesize, 0,
                        frame->height, data, linesize);

    if (ret <= 0)
    {
        return QImage();
    }

    QImage image(reinterpret_cast<uchar *>(m_rgbBuffer),
                 m_video.width, m_video.height,
                 m_video.width * 4, QImage::Format_RGB32);

    return image.copy();
}

bool FFmpegDecoderThread::takeScreenshot(const QString &filePath)
{
    OptionalFrameBuffer::VideoFrame frame = m_frameBuffer.pop(100);
    if (!frame.isValid())
        return false;
    return frame.image.save(filePath);
}

QImage FFmpegDecoderThread::convertYUVToImageStatic(const YUVFrameData &yuvData)
{
    if (!yuvData.isValid())
        return QImage();

    SwsContext *swsCtx = sws_getContext(
        yuvData.width, yuvData.height, AV_PIX_FMT_YUV420P,
        yuvData.width, yuvData.height, AV_PIX_FMT_RGB32,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!swsCtx)
        return QImage();

    int rgbSize = yuvData.width * yuvData.height * 4;
    uint8_t *rgbBuffer = new (std::nothrow) uint8_t[rgbSize];
    if (!rgbBuffer)
    {
        sws_freeContext(swsCtx);
        return QImage();
    }

    const uint8_t *srcData[3] = {
        reinterpret_cast<const uint8_t *>(yuvData.yData.constData()),
        reinterpret_cast<const uint8_t *>(yuvData.uData.constData()),
        reinterpret_cast<const uint8_t *>(yuvData.vData.constData())};
    int srcLinesize[3] = {yuvData.yLinesize, yuvData.uLinesize, yuvData.vLinesize};

    uint8_t *dstData[1] = {rgbBuffer};
    int dstLinesize[1] = {yuvData.width * 4};

    sws_scale(swsCtx, srcData, srcLinesize, 0, yuvData.height, dstData, dstLinesize);

    QImage image(yuvData.width, yuvData.height, QImage::Format_RGB32);
    memcpy(image.bits(), rgbBuffer, rgbSize);

    delete[] rgbBuffer;
    sws_freeContext(swsCtx);

    return image;
}

// ==================== 音频解码与处理 ====================

void FFmpegDecoderThread::decodeAudioPacket(AVPacket *pkt)
{
    // 增加空指针检查
    if (!m_audio.codecCtx)
    {
        return;
    }
    if (!pkt)
    {
        return;
    }

    int ret = avcodec_send_packet(m_audio.codecCtx, pkt);
    if (ret < 0)
        return;

    while (m_running)
    {
        // 再次检查上下文是否有效
        if (!m_audio.codecCtx || !m_audio.swrCtx)
            break;

        AVFrame *frame = av_frame_alloc();
        if (!frame)
            break;

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
        int maxSize = 15;
        int usage = static_cast<int>(m_frameBuffer.size() * 100.0f / maxSize);
        emit bufferStatusChanged(usage, m_frameBuffer.droppedFrames());
    }

    // 计算帧率
    qint64 now = av_gettime_relative();
    if (now - m_lastFpsCalcTime >= 1000000)
    {
        m_currentFps = m_framesSinceLastFpsCalc;
        m_framesSinceLastFpsCalc = 0;
        m_lastFpsCalcTime = now;
    }
}

qint64 FFmpegDecoderThread::duration() const
{
    if (!m_formatCtx || m_formatCtx->duration == AV_NOPTS_VALUE)
        return 0;
    return m_formatCtx->duration * 1000 / AV_TIME_BASE;
}

bool FFmpegDecoderThread::isPaused() const { return m_paused; }

void FFmpegDecoderThread::setPaused()
{
    QMutexLocker lock(&m_mutex);
    m_paused = !m_paused;
    if (!m_paused)
        m_pauseCondition.wakeAll();
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
    , m_displayWidget_(nullptr)
    , m_displayLabel_(nullptr)
    , m_playerCore_(new PlayerWidgetBase(this))
    , m_decoder_(new FFmpegDecoderThread(this))
    , m_useOpenGL(true)
    , m_openglAvailable(false)
    , m_firstFrameReceived(false)
    , m_isClosing(false)
{
#if OPENGL_AVAILABLE
    m_openglAvailable = OpenGLVideoWidget::isOpenGLAvailable();
    if (m_openglAvailable)
    {
        qDebug() << "OpenGL is available, will use hardware accelerated rendering";
    }
    else
    {
        qDebug() << "OpenGL not available, falling back to software rendering";
        m_useOpenGL = false;
    }
#else
    m_useOpenGL = false;
    qDebug() << "OpenGL support not compiled, using software rendering";
#endif

    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);

    initRenderWidget();
    setupConnections();

    qDebug() << "FFmpegPlayer created - OpenGL:" << m_useOpenGL;
}

FFmpegPlayer::~FFmpegPlayer()
{
    qDebug() << "FFmpegPlayer destructor called";
    m_isClosing = true;

    if (m_decoder_)
    {
        disconnect(m_decoder_, nullptr, this, nullptr);
        disconnect(m_decoder_, nullptr, m_playerCore_, nullptr);
    }
    disconnect(m_playerCore_, nullptr, this, nullptr);
    disconnect(m_playerCore_, nullptr, m_decoder_, nullptr);

    cleanupDecoder();

    qDebug() << "FFmpegPlayer destroyed";
}

void FFmpegPlayer::cleanupDecoder()
{
    if (m_decoder_)
    {
        qDebug() << "Cleaning up decoder...";

        // 第一步：断开所有信号连接，避免在清理过程中收到信号
        disconnect(m_decoder_, nullptr, this, nullptr);
        disconnect(m_decoder_, nullptr, m_playerCore_, nullptr);

        // 第二步：停止解码器并等待线程退出
        if (m_decoder_->isRunning())
        {
            qDebug() << "Closing decoder...";
            m_decoder_->close(true); // 等待线程完全退出
        }

        // 第三步：删除解码器对象
        m_decoder_->deleteLater();
        m_decoder_ = nullptr;

        qDebug() << "Decoder cleaned up";
    }
}

void FFmpegPlayer::closeEvent(QCloseEvent *event)
{
    qDebug() << "FFmpegPlayer closeEvent";
    m_isClosing = true;

    cleanupDecoder();

    {
        QMutexLocker lock(&m_frameMutex);
        m_currentFrame = QImage();
    }

#if OPENGL_AVAILABLE
    if (m_glWidget_)
    {
        m_glWidget_->clear();
    }
#endif

    if (m_displayLabel_)
    {
        m_displayLabel_->clear();
    }

    event->accept();
}

void FFmpegPlayer::hideEvent(QHideEvent *event)
{
    qDebug() << "FFmpegPlayer hideEvent";
    QWidget::hideEvent(event);
}

void FFmpegPlayer::showEvent(QShowEvent *event)
{
    qDebug() << "FFmpegPlayer showEvent";
    QWidget::showEvent(event);

    if (m_displayWidget_)
    {
        m_displayWidget_->show();
    }
}

void FFmpegPlayer::initRenderWidget()
{
    // 移除旧的显示组件
    if (m_displayWidget_)
    {
        m_displayWidget_->deleteLater();
        m_displayWidget_ = nullptr;
    }

#if OPENGL_AVAILABLE
    if (m_useOpenGL && m_openglAvailable)
    {
        // 使用OpenGL硬件加速渲染
        m_glWidget_ = new OpenGLVideoWidget(this);
        m_displayWidget_ = m_glWidget_;
        m_displayLabel_ = nullptr;
        m_displayWidget_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_displayWidget_->setAttribute(Qt::WA_OpaquePaintEvent);
        m_displayWidget_->setAutoFillBackground(false);
        qDebug() << "Using OpenGL renderer";
    }
    else
#endif
    {
        // 降级使用QLabel软件渲染
        m_displayLabel_ = new QLabel(this);
        m_displayLabel_->setAlignment(Qt::AlignCenter);
        m_displayLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_displayLabel_->setStyleSheet("background-color: black;");
        m_displayLabel_->setAttribute(Qt::WA_OpaquePaintEvent);
        m_displayWidget_ = m_displayLabel_;
        qDebug() << "Using QLabel software renderer";
    }

    m_displayWidget_->setGeometry(rect());
    m_displayWidget_->show();
}

void FFmpegPlayer::setUseOpenGL(bool enabled)
{
    if (m_useOpenGL == enabled)
        return;

    m_useOpenGL = enabled;

#if OPENGL_AVAILABLE
    if (enabled && !m_openglAvailable)
    {
        qWarning() << "Cannot enable OpenGL: not available on this system";
        return;
    }
#endif

    // 重新初始化渲染组件
    initRenderWidget();

    // 如果有当前帧，重新显示
    QMutexLocker lock(&m_frameMutex);
    if (!m_currentFrame.isNull())
    {
        updateFrame(m_currentFrame);
    }
}

void FFmpegPlayer::updateFrame(const QImage &image)
{
    if (image.isNull())
    {
        qWarning() << "FFmpegPlayer::updateFrame - null image";
        return;
    }
    if (m_isClosing)
    {
        qDebug() << "FFmpegPlayer::updateFrame - widget is closing, ignoring frame";
        return;
    }

    // 保存当前帧
    {
        QMutexLocker lock(&m_frameMutex);
        m_currentFrame = image;
    }

    // 更新显示
    updateDisplay();
}

void FFmpegPlayer::updateDisplay()
{
    if (m_isClosing)
        return;

    QMutexLocker lock(&m_frameMutex);
    if (m_currentFrame.isNull())
    {
        return;
    }

#if OPENGL_AVAILABLE
    // 优先使用OpenGL渲染
    if (m_useOpenGL && m_glWidget_ && m_glWidget_->isInitialized())
    {
        m_glWidget_->updateFrame(m_currentFrame);
        return;
    }
#endif

    // 降级使用QLabel软件渲染
    if (m_displayLabel_)
    {
        QPixmap pixmap = QPixmap::fromImage(m_currentFrame)
                             .scaled(m_displayLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_displayLabel_->setPixmap(pixmap);
    }
}

void FFmpegPlayer::resizeEvent(QResizeEvent *event)
{
    if (m_displayWidget_)
    {
        m_displayWidget_->setGeometry(rect());
    }

#if OPENGL_AVAILABLE
    // OpenGL模式下不需要手动调用updateDisplay，OpenGLWidget会自动处理
    if (!(m_useOpenGL && m_glWidget_))
#endif
    {
        updateDisplay();
    }

    QWidget::resizeEvent(event);
}

void FFmpegPlayer::paintEvent(QPaintEvent *event)
{
    // 只有当没有视频帧时才绘制提示信息
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
        painter.drawText(rect(), Qt::AlignCenter, tr("No Video"));
    }

    // 有视频帧时，让子组件处理渲染
    QWidget::paintEvent(event);
}

PlayerWidgetBase *FFmpegPlayer::PlayerCore() const { return m_playerCore_; }

void FFmpegPlayer::stop()
{
    qDebug() << "FFmpegPlayer::stop";

    if (m_decoder_)
    {
        m_decoder_->close(false);
    }

    // 清空当前帧
    {
        QMutexLocker lock(&m_frameMutex);
        m_currentFrame = QImage();
    }

    m_firstFrameReceived = false;

#if OPENGL_AVAILABLE
    if (m_glWidget_)
    {
        m_glWidget_->clear();
    }
#endif

    if (m_displayLabel_)
    {
        m_displayLabel_->clear();
    }

    update();
}

void FFmpegPlayer::setupConnections()
{
    // ========== 连接RGB帧信号 ==========
    connect(m_decoder_, &FFmpegDecoderThread::frameReady, this, [this](const QImage &image)
            {
                if (m_isClosing) return;
                if (!m_firstFrameReceived) {
                    m_firstFrameReceived = true;
                    qDebug() << "First RGB frame received, size:" << image.size();
                }
                updateFrame(image); }, Qt::QueuedConnection);

    // ========== 连接YUV帧信号 ==========
    connect(m_decoder_, &FFmpegDecoderThread::frameReadyYUV, this, [this](const YUVFrameData &yuvData)
            {
                if (m_isClosing) return;
                if (!yuvData.isValid()) {
                    qWarning() << "Received invalid YUV frame";
                    return;
                }
                if (!m_firstFrameReceived) {
                    m_firstFrameReceived = true;
                    qDebug() << "First YUV frame received, size:" 
                             << yuvData.width << "x" << yuvData.height;
                }

#if OPENGL_AVAILABLE
                // 优先使用OpenGL渲染YUV数据（性能最佳）
                if (m_useOpenGL && m_glWidget_ && m_glWidget_->isInitialized()) {
                    m_glWidget_->updateFrameYUV(
                        reinterpret_cast<const uint8_t*>(yuvData.yData.constData()),
                        reinterpret_cast<const uint8_t*>(yuvData.uData.constData()),
                        reinterpret_cast<const uint8_t*>(yuvData.vData.constData()),
                        yuvData.yLinesize, 
                        yuvData.uLinesize, 
                        yuvData.vLinesize,
                        yuvData.width, 
                        yuvData.height);
                    return;
                }
#endif
                // 如果没有OpenGL或OpenGL不可用，转换为RGB再显示
                QImage rgbImage = FFmpegDecoderThread::convertYUVToImageStatic(yuvData);
                if (!rgbImage.isNull()) {
                    updateFrame(rgbImage);
                } else {
                    qWarning() << "Failed to convert YUV to RGB";
                } }, Qt::QueuedConnection);

    // ========== 连接位置和时长信号 ==========
    connect(m_decoder_, &FFmpegDecoderThread::positionChanged, m_playerCore_, &PlayerWidgetBase::currentPosition, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::durationChanged, m_playerCore_, &PlayerWidgetBase::currentDuration, Qt::QueuedConnection);

    // ========== 连接播放状态信号 ==========
    connect(m_decoder_, &FFmpegDecoderThread::playbackRateChanged, m_playerCore_, &PlayerWidgetBase::currentPlaybackRate, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::playStateChanged, m_playerCore_, &PlayerWidgetBase::playStateChanged, Qt::QueuedConnection);

    // ========== 连接音量信号 ==========
    connect(m_decoder_, &FFmpegDecoderThread::volumeChanged, m_playerCore_, &PlayerWidgetBase::currentVolume, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::muteStateChanged, m_playerCore_, &PlayerWidgetBase::muteStateChanged, Qt::QueuedConnection);

    // ========== 连接错误和状态信号 ==========
    connect(m_decoder_, &FFmpegDecoderThread::errorOccurred, m_playerCore_, &PlayerWidgetBase::errorInfoShow, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::bufferStatusChanged, this, &FFmpegPlayer::onBufferStatusChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::networkReconnecting, this, &FFmpegPlayer::onNetworkReconnecting, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::statisticsUpdated, this, &FFmpegPlayer::onStatisticsUpdated, Qt::QueuedConnection);

    // ========== 连接播放控制信号（从PlayerCore到Decoder） ==========
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
    if (m_isClosing)
    {
        qWarning() << "FFmpegPlayer::play - widget is closing, ignoring play request";
        return;
    }

    if (url.isEmpty())
    {
        qWarning() << "FFmpegPlayer::play - empty URL";
        return;
    }

    qDebug() << "FFmpegPlayer::play - starting playback of:" << url;

    m_firstFrameReceived = false;

    // 清空当前显示的帧
    {
        QMutexLocker lock(&m_frameMutex);
        m_currentFrame = QImage();
    }

#if OPENGL_AVAILABLE
    if (m_useOpenGL && m_glWidget_)
    {
        if (!m_glWidget_->isVisible())
        {
            m_glWidget_->show();
        }

        // 等待OpenGL初始化完成
        int waitCount = 0;
        while (!m_glWidget_->isInitialized() && waitCount < 50)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(10);
            waitCount++;
        }

        if (!m_glWidget_->isInitialized())
        {
            qWarning() << "OpenGL widget failed to initialize, falling back to software rendering";
            setUseOpenGL(false);
        }
        else
        {
            qDebug() << "OpenGL widget initialized, ready for playback";
            m_glWidget_->clear();
        }
    }
#endif

    // ========== 确保之前的播放完全停止 ==========
    // 检查解码器是否正在运行
    if (m_decoder_ && m_decoder_->isRunning())
    {
        qDebug() << "Stopping previous playback before starting new one...";
        // 断开信号连接，避免旧播放的信号干扰
        disconnect(m_decoder_, &FFmpegDecoderThread::frameReady, this, nullptr);
        disconnect(m_decoder_, &FFmpegDecoderThread::frameReadyYUV, this, nullptr);

        // 关闭解码器并等待线程退出
        m_decoder_->close(true);

        // 重新连接信号
        connect(m_decoder_, &FFmpegDecoderThread::frameReady, this, [this](const QImage &image)
                {
                    if (m_isClosing) return;
                    if (!m_firstFrameReceived) {
                        m_firstFrameReceived = true;
                        qDebug() << "First RGB frame received, size:" << image.size();
                    }
                    updateFrame(image); }, Qt::QueuedConnection);

        connect(m_decoder_, &FFmpegDecoderThread::frameReadyYUV, this, [this](const YUVFrameData &yuvData)
                {
                    if (m_isClosing) return;
                    if (!yuvData.isValid()) return;
                    if (!m_firstFrameReceived) {
                        m_firstFrameReceived = true;
                        qDebug() << "First YUV frame received, size:" 
                                 << yuvData.width << "x" << yuvData.height;
                    }
#if OPENGL_AVAILABLE
                    if (m_useOpenGL && m_glWidget_ && m_glWidget_->isInitialized()) {
                        m_glWidget_->updateFrameYUV(
                            reinterpret_cast<const uint8_t*>(yuvData.yData.constData()),
                            reinterpret_cast<const uint8_t*>(yuvData.uData.constData()),
                            reinterpret_cast<const uint8_t*>(yuvData.vData.constData()),
                            yuvData.yLinesize, yuvData.uLinesize, yuvData.vLinesize,
                            yuvData.width, yuvData.height);
                        return;
                    }
#endif
                    QImage rgbImage = FFmpegDecoderThread::convertYUVToImageStatic(yuvData);
                    if (!rgbImage.isNull()) {
                        updateFrame(rgbImage);
                    } }, Qt::QueuedConnection);
    }

    // 打开新媒体
    bool success = m_decoder_->openMedia(url);
    if (!success)
    {
        qWarning() << "Failed to open media:" << url;
    }
}

void FFmpegPlayer::setFrameBufferEnabled(bool enabled)
{
    if (m_decoder_)
        m_decoder_->setFrameBufferEnabled(enabled);
}
void FFmpegPlayer::setMaxBufferSize(int size)
{
    if (m_decoder_)
        m_decoder_->setMaxBufferSize(size);
}
void FFmpegPlayer::setAutoReconnect(bool enabled, int maxRetries)
{
    if (m_decoder_)
        m_decoder_->setAutoReconnect(enabled, maxRetries);
}
void FFmpegPlayer::setMemoryLimit(int limitMB)
{
    if (m_decoder_)
        m_decoder_->setMemoryLimit(limitMB);
}
bool FFmpegPlayer::takeScreenshot(const QString &filePath)
{
    return m_decoder_ ? m_decoder_->takeScreenshot(filePath) : false;
}
FFmpegDecoderThread::Statistics FFmpegPlayer::getStatistics() const
{
    return m_decoder_ ? m_decoder_->getStatistics() : FFmpegDecoderThread::Statistics();
}
void FFmpegPlayer::setPlaybackRate(float rate)
{
    if (m_decoder_)
        m_decoder_->setPlaybackRate(rate);
}
float FFmpegPlayer::playbackRate() const
{
    return m_decoder_ ? m_decoder_->playbackRate() : 1.0f;
}

void FFmpegPlayer::onBufferStatusChanged(int usagePercent, int droppedFrames)
{
    if (droppedFrames > 0)
        qDebug() << "Buffer:" << usagePercent << "% Dropped:" << droppedFrames;
}

void FFmpegPlayer::onNetworkReconnecting(int attempt, int maxRetries)
{
    qDebug() << "Network reconnecting:" << attempt << "/" << maxRetries;
}

void FFmpegPlayer::onStatisticsUpdated(const FFmpegDecoderThread::Statistics &stats)
{
    Q_UNUSED(stats)
}

#endif // CAN_USE_FFMPEG