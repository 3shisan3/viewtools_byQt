#ifdef CAN_USE_FFMPEG

#include "ffmpeg_player_widget.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QPainter>
#include <QResizeEvent>
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
    m_audioBufferTimer->setInterval(50);  // 50ms间隔
    connect(m_audioBufferTimer, &QTimer::timeout, this, [this]() {
        m_audioWriter.notifyBufferReady();
    });
    
    qDebug() << "FFmpegDecoderThread initialized";
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
        if (!found) format = deviceInfo.nearestFormat(format);
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

    // 设置FFmpeg选项
    AVDictionary *options = nullptr;
    bool isNetwork = url.startsWith("rtsp://") || url.startsWith("http://");

    if (isNetwork)
    {
        // 网络流优化选项
        av_dict_set(&options, "rtsp_transport", "tcp", 0);      // RTSP使用TCP传输
        av_dict_set(&options, "stimeout", "5000000", 0);        // 超时5秒
        av_dict_set(&options, "max_delay", "500000", 0);        // 最大延迟500ms
        av_dict_set(&options, "buffer_size", "1048576", 0);     // 缓冲区1MB
        av_dict_set(&options, "reconnect", "1", 0);             // 启用重连
        av_dict_set(&options, "reconnect_at_eof", "1", 0);      // EOF时重连
        av_dict_set(&options, "reconnect_streamed", "1", 0);    // 流式重连
        av_dict_set(&options, "reconnect_delay_max", "5000000", 0); // 最大重连延迟5秒
    }
    else
    {
        // 本地文件优化选项
        av_dict_set(&options, "probesize", "32768", 0);         // 探测大小32KB
        av_dict_set(&options, "analyzeduration", "200000", 0);  // 分析时长200ms
    }

    // 打开输入流
    int ret = avformat_open_input(&m_formatCtx, url.toUtf8().constData(), nullptr, &options);
    if (options) av_dict_free(&options);

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
    if (m_audioBufferTimer) m_audioBufferTimer->start();

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
    if (!m_autoReconnect || m_isReconnecting) return false;

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
    if (!m_useBuffer || m_video.width <= 0) return;
    
    // 根据分辨率计算百万像素数
    int megaPixels = (m_video.width * m_video.height) / (1024 * 1024);
    int bufferSize;
    
    // 4K视频使用极小缓冲区，减少内存压力
    if (m_video.width >= _4K_WIDTH_THRESHOLD || m_video.height >= _4K_HEIGHT_THRESHOLD) {
        bufferSize = _4K_BUFFER_SIZE;
        qDebug() << "4K video detected - using ultra-small buffer:" << bufferSize << "frames";
        
        // 4K视频强制启用快速解码选项
        if (m_video.codecCtx) {
            m_video.codecCtx->thread_count = qMax(4, QThread::idealThreadCount());
            m_video.codecCtx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
        }
    }
    else if (megaPixels >= 4) {   // 2K (2560x1440 = 3.7MP)
        bufferSize = _1080P_BUFFER_SIZE;
        qDebug() << "2K video detected - buffer size:" << bufferSize;
    }
    else if (megaPixels >= 2) {   // 1080p
        bufferSize = _1080P_BUFFER_SIZE;
        qDebug() << "1080p video detected - buffer size:" << bufferSize;
    }
    else {                        // 低分辨率
        bufferSize = _LOW_RES_BUFFER_SIZE;
        qDebug() << "Low resolution video - buffer size:" << bufferSize;
    }
    
    m_frameBuffer.setMaxSize(bufferSize);
}

// ==================== 视频初始化 ====================

bool FFmpegDecoderThread::initVideo()
{
    // 查找最佳视频流
    m_video.streamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_video.streamIndex < 0) return false;

    // 获取解码器参数
    AVCodecParameters *params = m_formatCtx->streams[m_video.streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(params->codec_id);
    if (!codec) return false;

    // 分配解码器上下文
    m_video.codecCtx = avcodec_alloc_context3(codec);
    if (!m_video.codecCtx) return false;

    // 将参数填充到上下文
    if (avcodec_parameters_to_context(m_video.codecCtx, params) < 0)
    {
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    // ========== 4K性能优化：设置解码器选项 ==========
    AVDictionary *opts = nullptr;
    
    // 检查是否为4K视频
    bool is4K = (m_video.codecCtx->width >= _4K_WIDTH_THRESHOLD || 
                 m_video.codecCtx->height >= _4K_HEIGHT_THRESHOLD);
    
    if (is4K) {
        // 4K视频：启用多线程解码
        int threadCount = qMax(4, QThread::idealThreadCount());
        m_video.codecCtx->thread_count = threadCount;
        m_video.codecCtx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
        
        // 设置解码器选项以提升性能
        av_dict_set(&opts, "lowres", "0", 0);           // 保持原始分辨率
        av_dict_set(&opts, "fast", "1", 0);             // 启用快速解码
        av_dict_set(&opts, "skip_loop_filter", "1", 0); // 跳过环路滤波（牺牲少量画质）
        av_dict_set(&opts, "skip_idct", "0", 0);        // 保持IDCT质量
        av_dict_set(&opts, "skip_frame", "0", 0);       // 不跳帧
        
        qDebug() << "4K optimization enabled - thread count:" << threadCount;
    } else {
        // 普通视频：适度多线程
        m_video.codecCtx->thread_count = qMin(2, QThread::idealThreadCount());
        m_video.codecCtx->thread_type = FF_THREAD_SLICE;
    }

    // 打开解码器
    if (avcodec_open2(m_video.codecCtx, codec, &opts) < 0)
    {
        av_dict_free(&opts);
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }
    av_dict_free(&opts);

    // 获取视频尺寸和时间基
    m_video.width = m_video.codecCtx->width;
    m_video.height = m_video.codecCtx->height;
    m_video.timeBase = m_formatCtx->streams[m_video.streamIndex]->time_base;

    // 分辨率有效性检查
    if (m_video.width <= 0 || m_video.height <= 0 || 
        m_video.width > 8192 || m_video.height > 4320) {
        qCritical() << "Invalid video resolution:" << m_video.width << "x" << m_video.height;
        emit errorOccurred(tr("Invalid video resolution: %1x%2").arg(m_video.width).arg(m_video.height));
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }
    
    qDebug() << "Video resolution:" << m_video.width << "x" << m_video.height;
    
    // 动态调整缓冲区大小
    adjustBufferForResolution();
    updateFrameRate();

    // ========== 选择缩放算法 ==========
    int swsFlags;
    if (is4K) {
        // 4K使用最快算法，牺牲少量画质换取性能
        swsFlags = SWS_FAST_BILINEAR | SWS_ACCURATE_RND;
        qDebug() << "4K mode: using fastest scaling algorithm";
    } else if (m_video.width >= 1920) {
        // 1080p使用双线性滤波
        swsFlags = SWS_BILINEAR;
        qDebug() << "1080p mode: using bilinear scaling";
    } else {
        // 低分辨率使用高质量双三次插值
        swsFlags = SWS_BICUBIC;
        qDebug() << "Low resolution: using bicubic scaling";
    }

    // 创建图像缩放上下文（YUV → RGB32）
    m_video.swsCtx = sws_getContext(m_video.width, m_video.height,
        m_video.codecCtx->pix_fmt, m_video.width, m_video.height,
        AV_PIX_FMT_RGB32, swsFlags, nullptr, nullptr, nullptr);

    if (!m_video.swsCtx)
    {
        // 降级尝试使用默认算法
        m_video.swsCtx = sws_getContext(m_video.width, m_video.height,
            m_video.codecCtx->pix_fmt, m_video.width, m_video.height,
            AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr, nullptr);
        
        if (!m_video.swsCtx)
        {
            avcodec_free_context(&m_video.codecCtx);
            emit errorOccurred(tr("Failed to initialize video scaler"));
            return false;
        }
    }

    // ========== 内存分配 ==========
    m_rgbBufferSize = m_video.width * m_video.height * 4;  // RGB32每像素4字节
    
    // 检查内存需求（4K视频降低内存限制）
    int requiredMB = m_rgbBufferSize / (1024 * 1024);
    int effectiveLimit = is4K ? qMin(m_memoryLimitMB, 256) : m_memoryLimitMB;
    
    if (requiredMB > effectiveLimit) {
        qWarning() << "Video requires" << requiredMB << "MB, limit is" << effectiveLimit << "MB";
        emit errorOccurred(tr("Video requires too much memory: %1MB (limit: %2MB)")
                          .arg(requiredMB).arg(effectiveLimit));
        avcodec_free_context(&m_video.codecCtx);
        sws_freeContext(m_video.swsCtx);
        return false;
    }
    
    // 使用 nothrow 避免分配失败时崩溃
    m_rgbBuffer = new (std::nothrow) uint8_t[m_rgbBufferSize];
    if (!m_rgbBuffer)
    {
        qCritical() << "Failed to allocate RGB buffer of size:" << m_rgbBufferSize;
        emit errorOccurred(tr("Insufficient memory for video playback"));
        avcodec_free_context(&m_video.codecCtx);
        sws_freeContext(m_video.swsCtx);
        return false;
    }
    
    qDebug() << "Video initialized -" << m_video.width << "x" << m_video.height
             << "fps:" << (m_video.frameRate.num ? (double)m_video.frameRate.num / m_video.frameRate.den : 25)
             << "buffer:" << m_frameBuffer.size() << "frames"
             << "memory:" << requiredMB << "MB"
             << "4K mode:" << (is4K ? "ON" : "OFF");
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
    
    // 方法3: 从平均帧率推算
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
    {
        m_video.frameRate = stream->avg_frame_rate;
        m_frameInterval = 1000000 * m_video.frameRate.den / m_video.frameRate.num;
        qDebug() << "Frame rate from avg:" << (double)m_video.frameRate.num / m_video.frameRate.den << "fps";
        return;
    }
    
    // 默认值：25fps
    m_video.frameRate = {25, 1};
    m_frameInterval = 40000;  // 40ms = 25fps
    qDebug() << "Using default frame rate: 25fps";
}

// ==================== 音频初始化 ====================

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
    
    // 检查原始采样率是否合理
    if (m_audio.originalSampleRate < minSampleRate || 
        m_audio.originalSampleRate > 192000) {
        qWarning() << "Unusual audio sample rate:" << m_audio.originalSampleRate << "Hz";
    }
    
    // 计算目标采样率（考虑倍速）
    int targetRate = static_cast<int>(m_audio.originalSampleRate * m_playbackRate);
    targetRate = qBound(minSampleRate, targetRate, maxSampleRate);
    
    // 如果原始采样率已在合理范围内且倍速为1，保持原始采样率
    if (qFabs(m_playbackRate - 1.0f) < 0.01f && 
        m_audio.originalSampleRate >= minSampleRate && 
        m_audio.originalSampleRate <= maxSampleRate) {
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
        if (!found) format = deviceInfo.nearestFormat(format);
    }

    m_audio.output = new QAudioOutput(deviceInfo, format, this);
#else
    // Qt6音频设备处理
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
    for (int rate : standardRates) {
        if (safeRate == rate) {
            isValidRate = true;
            break;
        }
    }
    
    if (!isValidRate) {
        // 找到最接近的标准采样率
        int closestRate = 44100;
        int minDiff = abs(safeRate - 44100);
        for (int rate : standardRates) {
            int diff = abs(safeRate - rate);
            if (diff < minDiff) {
                minDiff = diff;
                closestRate = rate;
            }
        }
        safeRate = closestRate;
        qDebug() << "Adjusted audio sample rate to standard:" << safeRate << "Hz";
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
    // 先停止音频输出
    if (m_audio.output)
    {
        m_audio.output->stop();
        m_audio.output->deleteLater();
        m_audio.output = nullptr;
        m_audio.device = nullptr;
    }

    // 清理视频资源
    if (m_video.swsCtx)
    {
        sws_freeContext(m_video.swsCtx);
        m_video.swsCtx = nullptr;
    }
    
    if (m_video.codecCtx)
    {
        avcodec_free_context(&m_video.codecCtx);
    }
    
    // 清理音频资源
    if (m_audio.swrCtx)
    {
        swr_free(&m_audio.swrCtx);
        m_audio.swrCtx = nullptr;
    }
    
    if (m_audio.codecCtx)
    {
        avcodec_free_context(&m_audio.codecCtx);
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

    // 重置索引
    m_video.streamIndex = -1;
    m_audio.streamIndex = -1;
    m_hasAudio = false;
    m_video.frameRate = {0, 0};
    m_frameInterval = 40000;
    
    // 清空帧缓冲
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
    
    // 用于无音频模式的时间基准
    qint64 systemStartTime = 0;
    qint64 firstPts = -1;

    while (m_running)
    {
        // ==================== 处理暂停 ====================
        {
            QMutexLocker lock(&m_mutex);
            while (m_paused && m_running && !m_seekRequested)
            {
                m_pauseCondition.wait(&m_mutex);
            }
        }
        
        // ==================== 处理倍速变化 ====================
        if (m_needReinitAudio && m_hasAudio)
        {
            m_needReinitAudio = false;
            reinitAudioOutput();
        }

        // ==================== 处理跳转 ====================
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

        // ==================== 读取数据包 ====================
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

        // ==================== 计算实时码率 ====================
        qint64 now = av_gettime_relative();
        if (now - m_lastBitrateCalcTime >= 1000000)
        {
            qint64 bytesDiff = m_lastBitrateBytes;
            m_stats.currentBitrate = bytesDiff * 8 / ((now - m_lastBitrateCalcTime) / 1000000);
            m_lastBitrateCalcTime = now;
            m_lastBitrateBytes = 0;
        }
        m_lastBitrateBytes += pkt->size;

        // ==================== 分发数据包 ====================
        if (pkt->stream_index == m_video.streamIndex)
        {
            decodeVideoPacket(pkt);
        }
        else if (pkt->stream_index == m_audio.streamIndex && m_hasAudio)
        {
            decodeAudioPacket(pkt);
        }

        // ==================== 处理视频帧显示 ====================
        if (m_useBuffer)
        {
            OptionalFrameBuffer::VideoFrame frame = m_frameBuffer.pop(5);
            if (frame.isValid())
            {
                qint64 waitTime = 0;
                
                if (m_hasAudio && m_syncManager.isAudioClockValid())
                {
                    // 有音频且音频时钟有效：使用音频时钟同步
                    waitTime = m_syncManager.calculateWaitTime(frame.pts, frame.duration, m_playbackRate);
                }
                else if (m_hasAudio && !m_syncManager.isAudioClockValid())
                {
                    waitTime = 10000; // 等待10ms让音频时钟建立
                }
                else
                {
                    // 无音频：使用系统时钟同步
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
                
                // ========== 4K优化：降低最大等待时间 ==========
                bool is4K = (m_video.width >= _4K_WIDTH_THRESHOLD || m_video.height >= _4K_HEIGHT_THRESHOLD);
                if (waitTime > 0)
                {
                    qint64 maxWait;
                    if (is4K) {
                        // 4K视频：最多等待一帧时间的一半，避免解码落后
                        maxWait = static_cast<qint64>(frame.duration / m_playbackRate) * _4K_WAIT_TIME_FACTOR;
                        maxWait = qMax(maxWait, 10000LL);  // 至少等待10ms
                    } else {
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
                
                // 发送视频帧到UI线程
                emit frameReady(frame.image);
                emit positionChanged(frame.pts / 1000);
                m_stats.totalFramesDisplayed++;
                m_framesSinceLastFpsCalc++;
                m_syncManager.frameDisplayed();
                m_lastDisplayTime = av_gettime_relative();
            }
        }

        // ==================== 定期更新统计 ====================
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

        // ==================== 背压控制 ====================
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

// ==================== 视频解码与处理 ====================

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
    // 计算PTS（微秒）
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

    // 转换帧并加入缓冲区
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

    // YUV → RGB32 转换
    int ret = sws_scale(m_video.swsCtx, frame->data, frame->linesize, 0,
              frame->height, data, linesize);
    
    if (ret <= 0) {
        return QImage();
    }

    // 创建QImage并深拷贝数据
    QImage image(m_video.width, m_video.height, QImage::Format_RGB32);
    memcpy(image.bits(), m_rgbBuffer, m_rgbBufferSize);
    
    return image;
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

// ==================== 音频解码与处理 ====================

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

// ==================== 性能统计 ====================

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
    if (now - m_lastFpsCalcTime >= 1000000) {
        m_currentFps = m_framesSinceLastFpsCalc;
        m_framesSinceLastFpsCalc = 0;
        m_lastFpsCalcTime = now;
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
    , m_displayWidget_(nullptr)
    , m_displayLabel_(nullptr)
    , m_playerCore_(new PlayerWidgetBase(this))
    , m_decoder_(new FFmpegDecoderThread(this))
    , m_useOpenGL(true)                    // 默认尝试使用OpenGL
    , m_openglAvailable(false)
    , m_firstFrameReceived(false)          // 标记是否已收到第一帧
{
#if OPENGL_AVAILABLE
    // 运行时检测OpenGL可用性
    m_openglAvailable = OpenGLVideoWidget::isOpenGLAvailable();
    if (m_openglAvailable) {
        qDebug() << "OpenGL is available, will use hardware accelerated rendering";
    } else {
        qDebug() << "OpenGL not available, falling back to software rendering";
        m_useOpenGL = false;
    }
#else
    m_useOpenGL = false;
    qDebug() << "OpenGL support not compiled (OPENGL_ENABLE not defined), using software rendering";
#endif
    
    initRenderWidget();
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

void FFmpegPlayer::initRenderWidget()
{
    // 移除旧的显示组件
    if (m_displayWidget_) {
        m_displayWidget_->deleteLater();
        m_displayWidget_ = nullptr;
    }
    
#if OPENGL_AVAILABLE
    if (m_useOpenGL && m_openglAvailable) {
        // 使用OpenGL硬件加速渲染
        m_glWidget_ = new OpenGLVideoWidget(this);
        m_displayWidget_ = m_glWidget_;
        m_displayLabel_ = nullptr;
        m_displayWidget_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        qDebug() << "Using OpenGL renderer for video playback (hardware accelerated)";
    } else 
#endif
    {
        // 降级使用QLabel软件渲染
        m_displayLabel_ = new QLabel(this);
        m_displayLabel_->setAlignment(Qt::AlignCenter);
        m_displayLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_displayLabel_->setStyleSheet("background-color: black;");
        m_displayWidget_ = m_displayLabel_;
        qDebug() << "Using QLabel software renderer";
    }
}

void FFmpegPlayer::setUseOpenGL(bool enabled)
{
    if (m_useOpenGL == enabled) return;
    
    m_useOpenGL = enabled;
    
#if OPENGL_AVAILABLE
    if (enabled && !m_openglAvailable) {
        qWarning() << "Cannot enable OpenGL: not available on this system";
        return;
    }
#endif
    
    // 重新初始化渲染组件
    initRenderWidget();
    
    // 如果有当前帧，重新显示
    QMutexLocker lock(&m_frameMutex);
    if (!m_currentFrame.isNull()) {
        updateFrame(m_currentFrame);
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
    if (m_currentFrame.isNull()) {
        qDebug() << "updateDisplay: no frame available";
        return;
    }
    
#if OPENGL_AVAILABLE
    if (m_useOpenGL && m_glWidget_ && m_glWidget_->isInitialized()) {
        qDebug() << "Sending frame to OpenGL:" << m_currentFrame.size() 
                 << "format:" << m_currentFrame.format();
        m_glWidget_->updateFrame(m_currentFrame);
        return;
    } else {
        qDebug() << "OpenGL not ready: initialized=" 
                 << (m_glWidget_ ? m_glWidget_->isInitialized() : false);
    }
#endif
    
    // QLabel软件渲染
    if (m_displayLabel_) {
        QPixmap pixmap = QPixmap::fromImage(m_currentFrame)
            .scaled(m_displayLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_displayLabel_->setPixmap(pixmap);
    }
}

void FFmpegPlayer::resizeEvent(QResizeEvent *event)
{
    if (m_displayWidget_) {
        m_displayWidget_->resize(event->size());
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
        painter.drawText(rect(), Qt::AlignCenter, "No Video");
    }
    
    // 有视频帧时，让子组件处理渲染
    QWidget::paintEvent(event);
}

// ==================== 播放器控制接口 ====================

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

void FFmpegPlayer::setMemoryLimit(int limitMB)
{
    m_decoder_->setMemoryLimit(limitMB);
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
    // 解码器 → 播放器
    // 使用lambda确保OpenGL完全初始化后再处理第一帧
    connect(m_decoder_, &FFmpegDecoderThread::frameReady, this, 
            [this](const QImage &image) {
                if (!m_firstFrameReceived) {
                    m_firstFrameReceived = true;
#if OPENGL_AVAILABLE
                    // 确保OpenGL widget已完全初始化
                    if (m_useOpenGL && m_glWidget_) {
                        if (!m_glWidget_->isInitialized()) {
                            qDebug() << "OpenGL widget not yet initialized, retrying in 50ms";
                            QTimer::singleShot(50, this, [this, image]() {
                                updateFrame(image);
                            });
                            return;
                        }
                        qDebug() << "OpenGL widget initialized, starting frame rendering";
                    }
#endif
                }
                updateFrame(image);
            }, Qt::QueuedConnection);
    
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

    // UI控制 → 解码器
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
    // 重置第一帧标记
    m_firstFrameReceived = false;
    
#if OPENGL_AVAILABLE
    // 确保 OpenGL Widget 已初始化
    if (m_useOpenGL && m_glWidget_)
    {
        if (!m_glWidget_->isVisible())
        {
            m_glWidget_->show();
        }
        
        // 等待 OpenGL 初始化完成
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
            qDebug() << "OpenGL widget initialized successfully, ready for playback";
        }
    }
#endif

    if (m_decoder_->isRunning())
        m_decoder_->close();
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

#endif // CAN_USE_FFMPEG