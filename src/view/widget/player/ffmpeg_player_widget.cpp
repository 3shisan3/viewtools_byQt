#ifdef CAN_USE_FFMPEG

#include "ffmpeg_player_widget.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QResizeEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>
#include <QDir>

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

FFmpegDecoderThread::FFmpegDecoderThread(QObject *parent) 
    : QThread(parent)
    , m_frameBuffer(30)  // 默认30帧缓冲区
{
    avformat_network_init();
    av_log_set_callback(ffmpegLogCallback);
    av_log_set_level(AV_LOG_DEBUG);
}

FFmpegDecoderThread::~FFmpegDecoderThread()
{
    close(false); // 避免等待自身线程
    cleanupResources();
}

bool FFmpegDecoderThread::openMedia(const QString &url)
{
    close();
    
    QMutexLocker lock(&m_mutex);
    m_currentUrl = url;
    m_firstVideoPts = -1;
    m_videoStartTime = 0;
    m_lastDisplayTime = 0;
    m_syncManager.reset();
    m_frameBuffer.clear();
    
    // 重置统计信息
    m_totalFramesDecoded = 0;
    m_totalFramesDisplayed = 0;
    m_droppedFrames = 0;
    m_lastStatUpdate = QDateTime::currentMSecsSinceEpoch();

    // 打开输入流
    AVDictionary *options = nullptr;
    if (url.startsWith("rtsp://"))
    {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "5000000", 0);
        av_dict_set(&options, "buffer_size", "1048576", 0); // 1MB缓冲区
    }
    else
    {
        av_dict_set(&options, "probesize", "32768", 0);
        av_dict_set(&options, "analyzeduration", "200000", 0);
    }

    int ret = avformat_open_input(&m_formatCtx, url.toUtf8().constData(), nullptr, &options);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(tr("Failed to open media: %1").arg(errbuf));
        if (options) {
            av_dict_free(&options);
        }
        return false;
    }
    
    // 释放选项字典
    if (options) {
        av_dict_free(&options);
    }

    // 查找流信息
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0)
    {
        emit errorOccurred(tr("Could not find stream information"));
        return false;
    }

    // 初始化音视频流
    bool videoOk = initVideo();
    bool audioOk = initAudio();
    
    // 根据是否有音频设置同步模式
    if (m_hasAudio) {
        m_syncManager.setSyncMode(AVSyncManager::SYNC_AUDIO_MASTER);
    } else {
        m_syncManager.setSyncMode(AVSyncManager::SYNC_VIDEO_MASTER);
        qDebug() << "Audio not available, using video master clock";
    }

    if (!videoOk && !m_hasAudio)
    {
        emit errorOccurred(tr("No valid audio/video streams found"));
        return false;
    }

    // 获取总时长
    qint64 duration = (m_formatCtx->duration != AV_NOPTS_VALUE) ? m_formatCtx->duration * 1000 / AV_TIME_BASE : 0;
    emit durationChanged(duration);

    m_clockTimer.start();
    m_running = true;
    emit playStateChanged(PlayerWidgetBase::PlayingState);
    start();
    
    qDebug() << "Media opened successfully, video:" << videoOk 
             << "audio:" << m_hasAudio << "duration:" << duration << "ms";
    return true;
}

bool FFmpegDecoderThread::initVideo()
{
    m_video.streamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_video.streamIndex < 0)
    {
        qWarning() << "No video stream found";
        return false;
    }

    AVCodecParameters *params = m_formatCtx->streams[m_video.streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(params->codec_id);
    if (!codec)
    {
        qWarning() << "Unsupported video codec:" << avcodec_get_name(params->codec_id);
        return false;
    }

    m_video.codecCtx = avcodec_alloc_context3(codec);
    if (!m_video.codecCtx)
    {
        qWarning() << "Failed to allocate video codec context";
        return false;
    }
    
    if (avcodec_parameters_to_context(m_video.codecCtx, params) < 0)
    {
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    // 打开解码器
    if (avcodec_open2(m_video.codecCtx, codec, nullptr) < 0)
    {
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    // 获取视频信息
    m_video.width = m_video.codecCtx->width;
    m_video.height = m_video.codecCtx->height;
    
    // 获取帧率
    m_video.frameRate = av_guess_frame_rate(m_formatCtx, 
                                           m_formatCtx->streams[m_video.streamIndex], 
                                           nullptr);
    if (m_video.frameRate.num && m_video.frameRate.den)
    {
        qDebug() << "Video frame rate:" << (1.0 * m_video.frameRate.num / m_video.frameRate.den) << "fps";
    }

    // 初始化图像转换上下文
    m_video.swsCtx = sws_getContext(
        m_video.width, m_video.height, m_video.codecCtx->pix_fmt,
        m_video.width, m_video.height, AV_PIX_FMT_RGB32,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    
    if (!m_video.swsCtx)
    {
        qWarning() << "Failed to create sws context";
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }
    
    qDebug() << "Video initialized: " << m_video.width << "x" << m_video.height 
             << " codec:" << avcodec_get_name(params->codec_id);
    return true;
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
        qWarning() << "Unsupported audio codec:" << avcodec_get_name(params->codec_id);
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

    // 处理声道布局
    if (params->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
    {
        av_channel_layout_default(&m_audio.codecCtx->ch_layout, params->ch_layout.nb_channels);
    }

    // 打开解码器
    if (avcodec_open2(m_audio.codecCtx, codec, nullptr) < 0)
    {
        qWarning() << "Failed to open audio codec";
        avcodec_free_context(&m_audio.codecCtx);
        m_hasAudio = false;
        return true;
    }

    // 初始化重采样器
    m_audio.swrCtx = swr_alloc();
    if (!m_audio.swrCtx)
    {
        qWarning() << "Failed to allocate swr context";
        avcodec_free_context(&m_audio.codecCtx);
        m_hasAudio = false;
        return true;
    }
    
    av_opt_set_chlayout(m_audio.swrCtx, "in_chlayout", &m_audio.codecCtx->ch_layout, 0);
    av_opt_set_int(m_audio.swrCtx, "in_sample_rate", m_audio.codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "in_sample_fmt", m_audio.codecCtx->sample_fmt, 0);

    AVChannelLayout out_chlayout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout(m_audio.swrCtx, "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(m_audio.swrCtx, "out_sample_rate", m_out_sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if (swr_init(m_audio.swrCtx) < 0)
    {
        qWarning() << "Failed to initialize swr context";
        swr_free(&m_audio.swrCtx);
        avcodec_free_context(&m_audio.codecCtx);
        m_hasAudio = false;
        return true;
    }

    // 创建音频格式
    QAudioFormat format = createAudioFormat();
    
    // 检查音频设备可用性
#if QT_VERSION_MAJOR < 6
    QAudioDeviceInfo deviceInfo = QAudioDeviceInfo::defaultOutputDevice();
    if (deviceInfo.isNull())
    {
        qWarning() << "No audio output device available, audio disabled";
        m_hasAudio = false;
        return true;
    }
    
    if (!deviceInfo.isFormatSupported(format))
    {
        format = deviceInfo.nearestFormat(format);
        if (format.isValid())
        {
            m_out_sample_rate = format.sampleRate();
            qDebug() << "Using nearest audio format with sample rate:" << m_out_sample_rate;
        }
        else
        {
            qWarning() << "Cannot get valid audio format, audio disabled";
            m_hasAudio = false;
            return true;
        }
    }
    
    m_audio.output = new QAudioOutput(deviceInfo, format, this);
#else
    QAudioDevice deviceInfo = QMediaDevices::defaultAudioOutput();
    if (deviceInfo.isNull())
    {
        qWarning() << "No audio output device available, audio disabled";
        m_hasAudio = false;
        return true;
    }
    
    if (!deviceInfo.isFormatSupported(format))
    {
        // 尝试常见采样率
        int sampleRates[] = {48000, 44100, 32000, 24000, 22050, 16000};
        bool found = false;
        for (int sr : sampleRates)
        {
            format.setSampleRate(sr);
            if (deviceInfo.isFormatSupported(format))
            {
                m_out_sample_rate = sr;
                found = true;
                qDebug() << "Using alternative sample rate:" << m_out_sample_rate;
                break;
            }
        }
        
        if (!found)
        {
            qWarning() << "No supported audio format available, audio disabled";
            m_hasAudio = false;
            return true;
        }
    }
    
    m_audio.output = new QAudioSink(deviceInfo, format, this);
#endif

    m_audio.device = m_audio.output->start();
    if (!m_audio.device)
    {
        qWarning() << "Failed to start audio device, audio disabled";
        m_hasAudio = false;
        m_audio.output->deleteLater();
        m_audio.output = nullptr;
        return true;
    }
    
    m_audio.deviceReady = true;
    m_hasAudio = true;
    
    qDebug() << "Audio initialized successfully: channels:" << m_audio.codecCtx->ch_layout.nb_channels
             << " sample rate:" << m_audio.codecCtx->sample_rate
             << " output sample rate:" << m_out_sample_rate;
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
    if (!m_running)
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_running = false;
        m_paused = false;
        m_pauseCondition.wakeAll();
    }

    if (waitForFinished && this != QThread::currentThread())
    {
        wait(1000); // 最多等待1秒
    }
    
    cleanupResources();
    emit playStateChanged(PlayerWidgetBase::StoppedState);
}

void FFmpegDecoderThread::cleanupResources()
{
    // 清空帧缓冲区
    m_frameBuffer.clear();
    
    // 安全释放音频资源
    if (m_audio.output)
    {
        m_audio.output->stop();
        m_audio.output->deleteLater();
        m_audio.output = nullptr;
        m_audio.device = nullptr;
        m_audio.deviceReady = false;
    }
    
    // 释放音频重采样器
    if (m_audio.swrCtx)
    {
        swr_free(&m_audio.swrCtx);
        m_audio.swrCtx = nullptr;
    }
    
    // 释放音频解码器
    if (m_audio.codecCtx)
    {
        avcodec_free_context(&m_audio.codecCtx);
        m_audio.codecCtx = nullptr;
    }
    
    // 释放视频转换器
    if (m_video.swsCtx)
    {
        sws_freeContext(m_video.swsCtx);
        m_video.swsCtx = nullptr;
    }
    
    // 释放视频解码器
    if (m_video.codecCtx)
    {
        avcodec_free_context(&m_video.codecCtx);
        m_video.codecCtx = nullptr;
    }
    
    // 释放格式上下文
    if (m_formatCtx)
    {
        avformat_close_input(&m_formatCtx);
        avformat_free_context(m_formatCtx);
        m_formatCtx = nullptr;
    }
    
    m_hasAudio = false;
    m_video.streamIndex = -1;
    m_audio.streamIndex = -1;
}

void FFmpegDecoderThread::run()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        emit errorOccurred(tr("Failed to allocate packet"));
        return;
    }
    
    qint64 lastStatTime = QDateTime::currentMSecsSinceEpoch();
    
    while (m_running)
    {
        // 处理暂停状态
        {
            QMutexLocker lock(&m_mutex);
            while (m_paused && m_running)
            {
                m_pauseCondition.wait(&m_mutex);
            }
        }
        
        // 处理定位请求
        if (m_seekRequested)
        {
            QMutexLocker lock(&m_mutex);
            int64_t seek_target = av_rescale(m_seekPos, AV_TIME_BASE, 1000);
            int ret = av_seek_frame(m_formatCtx, -1, seek_target, AVSEEK_FLAG_BACKWARD);
            if (ret >= 0)
            {
                if (m_video.codecCtx)
                    avcodec_flush_buffers(m_video.codecCtx);
                if (m_audio.codecCtx)
                    avcodec_flush_buffers(m_audio.codecCtx);
                
                m_clockTimer.restart();
                m_firstVideoPts = -1;
                m_video.clock = m_seekPos * 1000;
                m_audio.clock = m_seekPos * 1000;
                m_frameBuffer.clear();
                m_syncManager.reset();
                
                emit positionChanged(m_seekPos);
                qDebug() << "Seek to:" << m_seekPos << "ms";
            }
            m_seekRequested = false;
            continue;
        }
        
        // 读取数据包
        av_packet_unref(pkt);
        int ret = av_read_frame(m_formatCtx, pkt);
        
        if (ret < 0)
        {
            if (ret == AVERROR_EOF)
            {
                // 播放结束
                emit positionChanged(duration());
                break;
            }
            
            // 网络流可能需要等待
            if (m_currentUrl.startsWith("rtsp://") || m_currentUrl.startsWith("http://"))
            {
                QThread::msleep(5);
            }
            else
            {
                QThread::msleep(1);
            }
            continue;
        }
        
        // 处理音视频包
        if (pkt->stream_index == m_video.streamIndex)
        {
            decodeVideoPacket(pkt);
        }
        else if (pkt->stream_index == m_audio.streamIndex)
        {
            decodeAudioPacket(pkt);
        }
        
        // 处理显示
        if (!m_frameBuffer.isEmpty())
        {
            FrameBuffer::VideoFrame frame = m_frameBuffer.pop(10); // 等待最多10ms
            
            if (frame.isValid())
            {
                // 音视频同步控制
                qint64 delay = m_syncManager.calculateFrameDelay(frame.pts);
                
                if (delay == -1)
                {
                    // 丢弃严重滞后的帧
                    m_droppedFrames++;
                    continue;
                }
                
                // 控制显示时机
                qint64 currentTime = QDateTime::currentMSecsSinceEpoch() * 1000;
                if (m_lastDisplayTime > 0 && delay > 0)
                {
                    QThread::usleep(static_cast<unsigned long>(delay));
                }
                
                // 显示帧
                emit frameReady(frame.image);
                m_totalFramesDisplayed++;
                m_lastDisplayTime = QDateTime::currentMSecsSinceEpoch() * 1000;
                
                // 更新位置
                emit positionChanged(frame.pts / 1000);
                m_syncManager.updateVideoClock(frame.pts);
            }
        }
        
        // 更新性能统计
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastStatTime >= 1000) // 每秒更新一次
        {
            updatePerformanceStats();
            lastStatTime = now;
        }
        
        // 动态调整缓冲区大小
        int bufferSize = m_frameBuffer.size();
        if (bufferSize > 25)
        {
            QThread::msleep(2); // 缓冲区较满，降低解码速度
        }
    }
    
    av_packet_free(&pkt);
    
    // 最终统计
    updatePerformanceStats();
    qDebug() << "Decoder thread stopped, total frames decoded:" << m_totalFramesDecoded
             << " displayed:" << m_totalFramesDisplayed << " dropped:" << m_droppedFrames;
}

void FFmpegDecoderThread::decodeVideoPacket(AVPacket *pkt)
{
    if (!m_video.codecCtx) return;
    
    int ret = avcodec_send_packet(m_video.codecCtx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
    {
        return;
    }
    
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
        
        m_totalFramesDecoded++;
        processVideoFrame(frame);
        av_frame_free(&frame);
    }
}

void FFmpegDecoderThread::processVideoFrame(AVFrame *frame)
{
    // 计算时间戳
    qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
    pts = av_rescale_q(pts, m_formatCtx->streams[m_video.streamIndex]->time_base, {1, 1000000});
    
    // 记录首帧时间戳
    if (m_firstVideoPts == -1)
    {
        m_firstVideoPts = pts;
        m_videoStartTime = QDateTime::currentMSecsSinceEpoch() * 1000;
        qDebug() << "First video frame PTS:" << m_firstVideoPts << "us";
    }
    
    // 计算帧显示时长
    qint64 frameDuration = 40000; // 默认40ms
    if (m_video.frameRate.num && m_video.frameRate.den)
    {
        frameDuration = 1000000 * m_video.frameRate.den / m_video.frameRate.num;
    }
    
    // 转换图像格式
    QImage image = convertFrameToImage(frame);
    if (image.isNull())
    {
        return;
    }
    
    // 添加到缓冲区
    FrameBuffer::VideoFrame videoFrame(image, pts, frameDuration);
    m_frameBuffer.push(videoFrame);
    
    // 更新缓冲区状态
    int bufferUsage = static_cast<int>(m_frameBuffer.usage() * 100);
    if (bufferUsage > 80 || m_frameBuffer.droppedFrames() > 0)
    {
        emit bufferStatusChanged(bufferUsage, m_frameBuffer.droppedFrames());
    }
}

QImage FFmpegDecoderThread::convertFrameToImage(AVFrame *frame)
{
    if (!m_video.swsCtx || !frame) return QImage();
    
    // 创建RGB32帧
    AVFrame *rgbFrame = av_frame_alloc();
    if (!rgbFrame) return QImage();
    
    rgbFrame->format = AV_PIX_FMT_RGB32;
    rgbFrame->width = frame->width;
    rgbFrame->height = frame->height;
    
    if (av_frame_get_buffer(rgbFrame, 0) < 0)
    {
        av_frame_free(&rgbFrame);
        return QImage();
    }
    
    // 转换颜色空间
    sws_scale(m_video.swsCtx, frame->data, frame->linesize, 0,
              frame->height, rgbFrame->data, rgbFrame->linesize);
    
    // 创建QImage（不复制数据）
    QImage image(rgbFrame->data[0], rgbFrame->width, rgbFrame->height,
                 QImage::Format_RGB32,
                 [](void *ptr) {
                     AVFrame *frame = static_cast<AVFrame*>(ptr);
                     av_frame_free(&frame);
                 },
                 rgbFrame);
    
    return image.copy(); // 返回深拷贝，避免数据竞争
}

void FFmpegDecoderThread::decodeAudioPacket(AVPacket *pkt)
{
    if (!m_hasAudio || !m_audio.codecCtx) return;
    
    int ret = avcodec_send_packet(m_audio.codecCtx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
    {
        return;
    }
    
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
        
        // 计算时间戳
        qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
        pts = av_rescale_q(pts, m_formatCtx->streams[m_audio.streamIndex]->time_base, {1, 1000000});
        
        // 更新音频时钟
        m_syncManager.updateAudioClock(pts);
        m_audio.clock = pts;
        
        // 计算输出样本数
        int out_samples = av_rescale_rnd(
            swr_get_delay(m_audio.swrCtx, frame->sample_rate) + frame->nb_samples,
            m_out_sample_rate,
            frame->sample_rate,
            AV_ROUND_UP);
        
        // 分配输出缓冲区
        uint8_t *output = nullptr;
        int linesize = 0;
        if (av_samples_alloc(&output, &linesize, 2, out_samples, AV_SAMPLE_FMT_S16, 0) < 0)
        {
            av_frame_free(&frame);
            break;
        }
        
        // 执行重采样
        int realSamples = swr_convert(m_audio.swrCtx, &output, out_samples,
                                     (const uint8_t **)frame->data, frame->nb_samples);
        
        if (realSamples > 0)
        {
            // 应用音量和静音
            if (m_muted)
            {
                memset(output, 0, realSamples * 4);
            }
            else if (m_volume < 0.99f || m_volume > 1.01f)
            {
                applyVolume(output, realSamples * 4);
            }
            
            // 写入音频数据
            writeAudioData(output, realSamples * 4);
        }
        
        av_freep(&output);
        av_frame_free(&frame);
    }
}

bool FFmpegDecoderThread::writeAudioData(const uint8_t *data, qint64 size)
{
    if (!m_audio.device || !m_audio.output || !m_audio.deviceReady) return false;
    
    qint64 bytesWritten = 0;
    const qint64 CHUNK_SIZE = 4096; // 4KB chunks
    
    while (bytesWritten < size && m_running)
    {
        qint64 freeBytes = m_audio.output->bytesFree();
        if (freeBytes <= 0)
        {
            QThread::usleep(1000);
            continue;
        }
        
        qint64 remaining = size - bytesWritten;
        qint64 toWrite = qMin(qMin(freeBytes, CHUNK_SIZE), remaining);
        
        qint64 written = m_audio.device->write(
            reinterpret_cast<const char*>(data + bytesWritten),
            toWrite);
        
        if (written > 0)
        {
            bytesWritten += written;
        }
        else
        {
            break;
        }
    }
    
    return bytesWritten == size;
}

void FFmpegDecoderThread::applyVolume(uint8_t *data, int len)
{
    int16_t *samples = reinterpret_cast<int16_t *>(data);
    const int count = len / sizeof(int16_t);
    
    for (int i = 0; i < count; ++i)
    {
        float sample = samples[i] * m_volume;
        samples[i] = static_cast<int16_t>(qBound(-32768.0f, sample, 32767.0f));
    }
}

qint64 FFmpegDecoderThread::getMasterClock() const
{
    return m_syncManager.getMasterClock();
}

void FFmpegDecoderThread::updateVideoClock(qint64 pts)
{
    m_video.clock = pts;
}

void FFmpegDecoderThread::updateAudioClock(qint64 pts)
{
    m_audio.clock = pts;
}

qint64 FFmpegDecoderThread::duration() const
{
    if (!m_formatCtx || m_formatCtx->duration == AV_NOPTS_VALUE)
    {
        return 0;
    }
    return m_formatCtx->duration * 1000 / AV_TIME_BASE;
}

bool FFmpegDecoderThread::isPaused() const
{
    return m_paused;
}

void FFmpegDecoderThread::setPaused()
{
    QMutexLocker locker(&m_mutex);
    m_paused = !m_paused;
    if (!m_paused)
    {
        m_pauseCondition.wakeAll();
    }
    emit playStateChanged(m_paused ? PlayerWidgetBase::PausedState : PlayerWidgetBase::PlayingState);
}

void FFmpegDecoderThread::seekTo(qint64 posMs)
{
    QMutexLocker locker(&m_mutex);
    m_seekPos = qBound(0LL, posMs, duration());
    m_seekRequested = true;
    m_frameBuffer.clear(); // 清空缓冲区
    qDebug() << "Seek requested to:" << m_seekPos << "ms";
}

void FFmpegDecoderThread::setVolume(float volume)
{
    volume = qBound(0.0f, volume / 100.0f, 1.0f);
    m_volume = volume;
    emit volumeChanged(static_cast<int>(volume * 100));
}

void FFmpegDecoderThread::setMute()
{
    QMutexLocker lock(&m_mutex);
    m_muted = !m_muted;
    emit muteStateChanged(m_muted);
}

void FFmpegDecoderThread::updatePerformanceStats()
{
    int bufferUsage = static_cast<int>(m_frameBuffer.usage() * 100);
    int droppedFrames = m_frameBuffer.droppedFrames();
    
    emit bufferStatusChanged(bufferUsage, droppedFrames);
}

// FFmpegPlayer 实现
FFmpegPlayer::FFmpegPlayer(QWidget *parent) 
    : QWidget(parent)
    , m_displayLabel_(new QLabel(this))
    , m_playerCore_(new PlayerWidgetBase(this))
    , m_decoder_(new FFmpegDecoderThread(this))
{
    m_displayLabel_->setAlignment(Qt::AlignCenter);
    m_displayLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_displayLabel_->setAttribute(Qt::WA_OpaquePaintEvent);
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

PlayerWidgetBase *FFmpegPlayer::PlayerCore() const
{
    return m_playerCore_;
}

void FFmpegPlayer::setupConnections()
{
    // 使用QueuedConnection确保线程安全
    connect(m_decoder_, &FFmpegDecoderThread::frameReady, this, &FFmpegPlayer::updateFrame, Qt::QueuedConnection);
    
    // 解码器 -> 播放器核心
    connect(m_decoder_, &FFmpegDecoderThread::positionChanged, m_playerCore_, &PlayerWidgetBase::currentPosition, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::durationChanged, m_playerCore_, &PlayerWidgetBase::currentDuration, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::playStateChanged, m_playerCore_, &PlayerWidgetBase::playStateChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::volumeChanged, m_playerCore_, &PlayerWidgetBase::currentVolume, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::muteStateChanged, m_playerCore_, &PlayerWidgetBase::muteStateChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::errorOccurred, m_playerCore_, &PlayerWidgetBase::errorInfoShow, Qt::QueuedConnection);
    
    // 性能监控
    connect(m_decoder_, &FFmpegDecoderThread::bufferStatusChanged, this, &FFmpegPlayer::onBufferStatusChanged, Qt::QueuedConnection);
    
    // 播放器核心 -> 解码器
    connect(m_playerCore_, &PlayerWidgetBase::setPlayerFile, this, &FFmpegPlayer::play, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::setPlayerUrl, this, &FFmpegPlayer::play, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::changePlayState, m_decoder_, &FFmpegDecoderThread::setPaused, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::changeMuteState, m_decoder_, &FFmpegDecoderThread::setMute, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::seekPlay, m_decoder_, &FFmpegDecoderThread::seekTo, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::setVolume, m_decoder_, &FFmpegDecoderThread::setVolume, Qt::QueuedConnection);
}

void FFmpegPlayer::play(const QString &url)
{
    if (m_decoder_->isRunning())
    {
        m_decoder_->close();
    }
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
    
    updateDisplayLabel();
    update();
}

void FFmpegPlayer::updateDisplayLabel()
{
    QMutexLocker lock(&m_frameMutex);
    if (m_currentFrame.isNull()) return;
    
    QPixmap pixmap = QPixmap::fromImage(m_currentFrame)
        .scaled(m_displayLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    
    m_displayLabel_->setPixmap(pixmap);
}

void FFmpegPlayer::onBufferStatusChanged(int usagePercent, int droppedFrames)
{
    // 可以在这里添加日志或状态显示
    if (droppedFrames > 0)
    {
        qDebug() << "Buffer status - Usage:" << usagePercent << "% Dropped:" << droppedFrames;
    }
}

void FFmpegPlayer::resizeEvent(QResizeEvent *event)
{
    m_displayLabel_->resize(event->size());
    updateDisplayLabel();
    QWidget::resizeEvent(event);
}

void FFmpegPlayer::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    
    // 检查是否显示有图片
    bool hasVideo = false;
    
    // 方法1：检查当前帧
    {
        QMutexLocker lock(&m_frameMutex);
        hasVideo = !m_currentFrame.isNull();
    }
    
    // 方法2：检查显示标签是否有图片
    if (!hasVideo)
    {
        // 兼容 Qt5 的不同版本
        #if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        hasVideo = !m_displayLabel_->pixmap(Qt::ReturnByValue).isNull();
        #else
        const QPixmap *pixmap = m_displayLabel_->pixmap();
        hasVideo = (pixmap != nullptr && !pixmap->isNull());
        #endif
    }
    
    // 如果没有视频，显示黑色背景和提示文字
    if (!hasVideo)
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, "No Video");
    }
}

bool FFmpegPlayer::event(QEvent *event)
{
    if (event->type() == QEvent::LayoutRequest)
    {
        updateDisplayLabel();
    }
    return QWidget::event(event);
}

#endif