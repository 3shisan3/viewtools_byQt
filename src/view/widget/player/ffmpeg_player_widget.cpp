#ifdef CAN_USE_FFMPEG

#include "ffmpeg_player_widget.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QResizeEvent>

// FFmpeg日志回调
static void ffmpegLogCallback(void *, int level, const char *fmt, va_list vl)
{
    if (level <= AV_LOG_WARNING)
    {
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), fmt, vl);
        char *p = buffer;
        while (*p) {
            if (*p == '\n' || *p == '\r') *p = ' ';
            p++;
        }
        qDebug() << "FFmpeg:" << buffer;
    }
}

FFmpegDecoderThread::FFmpegDecoderThread(QObject *parent) : QThread(parent)
{
    avformat_network_init();
    av_log_set_callback(ffmpegLogCallback);
    av_log_set_level(AV_LOG_WARNING);
}

FFmpegDecoderThread::~FFmpegDecoderThread()
{
    close(false); // 避免等待自身线程
}

bool FFmpegDecoderThread::openMedia(const QString &url)
{
    close();
    
    QMutexLocker lock(&m_mutex);
    m_seekRequested = false;
    m_firstVideoPts = -1;
    m_videoStartTime = 0;
    m_videoClock = 0;
    m_audioClock = 0;
    m_lastFrameTime = 0;

    // 打开输入流
    AVDictionary *options = nullptr;
    if (url.startsWith("rtsp://"))
    {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "5000000", 0);
        av_dict_set(&options, "max_delay", "500000", 0);
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
        return false;
    }

    // 查找流信息
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0)
    {
        emit errorOccurred(tr("Could not find stream information"));
        return false;
    }

    // 初始化音视频流
    bool hasVideo = initVideo();
    bool hasAudio = initAudio();

    if (!hasVideo && !hasAudio)
    {
        emit errorOccurred(tr("No valid audio/video streams found"));
        return false;
    }

    if (hasAudio && !m_audio.output)
    {
        qWarning() << "Audio output initialization failed, video only mode";
    }

    qint64 duration = (m_formatCtx->duration != AV_NOPTS_VALUE) ? m_formatCtx->duration * 1000 / AV_TIME_BASE : 0;
    emit durationChanged(duration);

    m_running = true;
    m_paused = false;
    emit playStateChanged(PlayerWidgetBase::PlayingState);
    start();
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
        qWarning() << "Unsupported video codec";
        return false;
    }

    m_video.codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_video.codecCtx, params);

    // 打开解码器
    if (avcodec_open2(m_video.codecCtx, codec, nullptr) < 0)
    {
        avcodec_free_context(&m_video.codecCtx);
        return false;
    }

    // 计算帧间隔
    AVRational frameRate = av_guess_frame_rate(m_formatCtx,
                                               m_formatCtx->streams[m_video.streamIndex],
                                               nullptr);
    if (frameRate.den && frameRate.num)
    {
        m_frameInterval = 1000000 * frameRate.den / frameRate.num;
        qDebug() << "Frame rate:" << frameRate.num << "/" << frameRate.den
                 << "Interval:" << m_frameInterval << "us";
    }

    m_video.swsCtx = sws_getContext(
        m_video.codecCtx->width, m_video.codecCtx->height, m_video.codecCtx->pix_fmt,
        m_video.codecCtx->width, m_video.codecCtx->height, AV_PIX_FMT_RGB32,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    qDebug() << "Video initialized:" << m_video.codecCtx->width << "x" << m_video.codecCtx->height;
    return true;
}

bool FFmpegDecoderThread::initAudio()
{
    m_audio.streamIndex = av_find_best_stream(m_formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_audio.streamIndex < 0)
    {
        qWarning() << "No audio stream found";
        return false;
    }

    AVCodecParameters *params = m_formatCtx->streams[m_audio.streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(params->codec_id);
    if (!codec)
    {
        qWarning() << "Unsupported audio codec";
        return false;
    }

    m_audio.codecCtx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(m_audio.codecCtx, params) < 0)
    {
        avcodec_free_context(&m_audio.codecCtx);
        return false;
    }

    if (m_audio.codecCtx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
    {
        av_channel_layout_default(&m_audio.codecCtx->ch_layout, 
                                   m_audio.codecCtx->ch_layout.nb_channels);
    }

    if (avcodec_open2(m_audio.codecCtx, codec, nullptr) < 0)
    {
        avcodec_free_context(&m_audio.codecCtx);
        return false;
    }

    m_audio.swrCtx = swr_alloc();
    if (!m_audio.swrCtx)
    {
        return false;
    }

    AVChannelLayout out_chlayout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout(m_audio.swrCtx, "in_chlayout", &m_audio.codecCtx->ch_layout, 0);
    av_opt_set_int(m_audio.swrCtx, "in_sample_rate", m_audio.codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "in_sample_fmt", m_audio.codecCtx->sample_fmt, 0);
    av_opt_set_chlayout(m_audio.swrCtx, "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(m_audio.swrCtx, "out_sample_rate", m_out_sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if (swr_init(m_audio.swrCtx) < 0)
    {
        swr_free(&m_audio.swrCtx);
        return false;
    }

    m_audio.bytesPerSample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16) * 2;
    return initAudioOutput();
}

bool FFmpegDecoderThread::initAudioOutput()
{
    QAudioFormat format;
#if QT_VERSION_MAJOR < 6
    format.setSampleRate(m_out_sample_rate);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
    
    QAudioDeviceInfo deviceInfo(QAudioDeviceInfo::defaultOutputDevice());
    if (deviceInfo.isNull())
    {
        qWarning() << "No audio output device available";
        return false;
    }
    
    if (!deviceInfo.isFormatSupported(format))
    {
        format = deviceInfo.nearestFormat(format);
        m_out_sample_rate = format.sampleRate();
        qDebug() << "Using nearest audio format, sample rate:" << m_out_sample_rate;
    }
    
    m_audio.output = new QAudioOutput(deviceInfo, format, this);
#else
    format.setSampleRate(m_out_sample_rate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Int16);
    
    QAudioDevice deviceInfo(QMediaDevices::defaultAudioOutput());
    if (!deviceInfo.isFormatSupported(format))
    {
        qDebug() << "Audio format not supported, using nearest";
    }
    m_audio.output = new QAudioSink(deviceInfo, format, this);
#endif

    if (!m_audio.output)
    {
        qWarning() << "Failed to create audio output";
        return false;
    }

    m_audio.device = m_audio.output->start();
    if (!m_audio.device)
    {
        qWarning() << "Failed to start audio device";
        delete m_audio.output;
        m_audio.output = nullptr;
        return false;
    }

    qDebug() << "Audio output initialized successfully";
    return true;
}

void FFmpegDecoderThread::close(bool waitForFinished)
{
    if (!m_running && !isRunning())
        return;

    {
        QMutexLocker lock(&m_mutex);
        m_running = false;
        m_paused = false;
        m_pauseCondition.wakeAll();
    }

    if (waitForFinished && this != QThread::currentThread())
    {
        wait();
    }

    if (m_audio.output)
    {
        if (m_audio.device)
        {
            m_audio.device->close();
        }
        m_audio.output->stop();
        m_audio.output->deleteLater();
        m_audio.output = nullptr;
        m_audio.device = nullptr;
    }

    if (m_video.swsCtx)
    {
        sws_freeContext(m_video.swsCtx);
        m_video.swsCtx = nullptr;
    }
    if (m_video.codecCtx)
    {
        avcodec_free_context(&m_video.codecCtx);
    }

    if (m_audio.swrCtx)
    {
        swr_free(&m_audio.swrCtx);
    }
    if (m_audio.codecCtx)
    {
        avcodec_free_context(&m_audio.codecCtx);
    }

    if (m_formatCtx)
    {
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }

    emit playStateChanged(PlayerWidgetBase::StoppedState);
}

void FFmpegDecoderThread::run()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return;

    while (m_running)
    {
        // 处理暂停
        {
            QMutexLocker lock(&m_mutex);
            while (m_paused && m_running)
            {
                m_pauseCondition.wait(&m_mutex);
            }
        }

        // 处理跳转
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
                m_firstVideoPts = -1;
                m_videoClock = m_seekPos / 1000.0;
                m_audioClock = m_seekPos / 1000.0;
                emit positionChanged(m_seekPos);
                m_lastFrameTime = 0;
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
                emit positionChanged(duration());
                break;
            }
            QThread::msleep(5);
            continue;
        }

        // 视频包处理
        if (pkt->stream_index == m_video.streamIndex)
        {
            decodeVideoPacket(pkt);
        }
        // 音频包处理
        else if (pkt->stream_index == m_audio.streamIndex && m_audio.output)
        {
            decodeAudioPacket(pkt);
        }
    }

    av_packet_free(&pkt);
    close(false);
}

void FFmpegDecoderThread::decodeVideoPacket(AVPacket *pkt)
{
    int ret = avcodec_send_packet(m_video.codecCtx, pkt);
    if (ret < 0)
        return;

    while (true)
    {
        AVFrame *frame = av_frame_alloc();
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

        displayVideoFrame(frame);
        av_frame_free(&frame);
    }
}

void FFmpegDecoderThread::displayVideoFrame(AVFrame *frame)
{
    // 计算PTS（微秒）
    qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
    pts = av_rescale_q(pts,
                       m_formatCtx->streams[m_video.streamIndex]->time_base,
                       {1, 1000000});

    // 记录首帧PTS和系统时间
    if (m_firstVideoPts == -1)
    {
        m_firstVideoPts = pts;
        m_videoStartTime = av_gettime_relative();
        qDebug() << "First video frame PTS:" << m_firstVideoPts;
    }

    // 计算应该显示的时间
    qint64 currentTime = av_gettime_relative();
    qint64 expectedTime = m_videoStartTime + (pts - m_firstVideoPts);
    qint64 delay = expectedTime - currentTime;

    // 音视频同步（如果有音频）
    if (m_audio.output && m_audioClock > 0)
    {
        double videoTime = pts / 1000000.0;
        delay = (videoTime - m_audioClock) * 1000000;
    }

    // 同步控制
    if (delay > 5000)
    {
        // 需要等待，但不超过50ms
        int waitUs = qMin((int)delay, 50000);
        QThread::usleep(waitUs);
    }
    else if (delay < -50000)
    {
        // 视频落后太多，跳过该帧
        static int skipCount = 0;
        if (skipCount++ % 30 == 0)
        {
            qDebug() << "Skipping late frame, delay:" << delay / 1000 << "ms";
        }
        return;
    }

    // 更新视频时钟
    m_videoClock = pts / 1000000.0;

    // 转换为RGB32
    uint8_t *buffer = new uint8_t[m_video.codecCtx->width * m_video.codecCtx->height * 4];
    uint8_t *data[1] = {buffer};
    int linesize[1] = {m_video.codecCtx->width * 4};

    sws_scale(m_video.swsCtx, frame->data, frame->linesize, 0,
              frame->height, data, linesize);

    QImage image(buffer, m_video.codecCtx->width, m_video.codecCtx->height, 
                 QImage::Format_RGB32, [](void *ptr) { delete[] static_cast<uint8_t*>(ptr); }, 
                 buffer);

    emit frameReady(image.copy());
    emit positionChanged(pts / 1000);
    
    // 帧间延迟控制
    qint64 now = av_gettime_relative();
    if (m_lastFrameTime > 0)
    {
        qint64 elapsed = now - m_lastFrameTime;
        if (elapsed < m_frameInterval)
        {
            QThread::usleep(m_frameInterval - elapsed);
        }
    }
    m_lastFrameTime = av_gettime_relative();
}

void FFmpegDecoderThread::decodeAudioPacket(AVPacket *pkt)
{
    if (!m_audio.swrCtx || !m_audio.codecCtx || !m_audio.device)
        return;

    int ret = avcodec_send_packet(m_audio.codecCtx, pkt);
    if (ret < 0)
        return;

    while (m_running)
    {
        AVFrame *frame = av_frame_alloc();
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

        // 计算PTS
        qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
        pts = av_rescale_q(pts,
                          m_formatCtx->streams[m_audio.streamIndex]->time_base,
                          {1, 1000000});

        // 更新音频时钟
        m_audioClock = pts / 1000000.0;

        // 计算输出样本数
        int out_samples = av_rescale_rnd(
            swr_get_delay(m_audio.swrCtx, frame->sample_rate) + frame->nb_samples,
            m_out_sample_rate,
            frame->sample_rate,
            AV_ROUND_UP);

        // 分配输出缓冲区
        uint8_t *output = nullptr;
        if (av_samples_alloc(&output, nullptr, 2, out_samples, AV_SAMPLE_FMT_S16, 0) < 0)
        {
            av_frame_free(&frame);
            break;
        }

        // 执行重采样
        int realSamples = swr_convert(m_audio.swrCtx, &output, out_samples,
                                      (const uint8_t **)frame->data, frame->nb_samples);

        if (realSamples > 0)
        {
            int dataSize = realSamples * m_audio.bytesPerSample;
            
            // 音量控制
            if (m_volume != 100)
            {
                float volumeFactor = m_volume / 100.0f;
                int16_t *samples = reinterpret_cast<int16_t*>(output);
                int sampleCount = dataSize / sizeof(int16_t);
                for (int i = 0; i < sampleCount; ++i)
                {
                    samples[i] = static_cast<int16_t>(qBound(-32768.0f, 
                                                              samples[i] * volumeFactor, 
                                                              32767.0f));
                }
            }
            
            // 静音处理
            if (m_muted)
            {
                memset(output, 0, dataSize);
            }
            
            // 写入音频数据
            qint64 bytesWritten = 0;
            while (bytesWritten < dataSize && m_running)
            {
                qint64 bytesFree = m_audio.output->bytesFree();
                if (bytesFree <= 0)
                {
                    QThread::usleep(1000);
                    continue;
                }
                
                qint64 toWrite = qMin((qint64)(dataSize - bytesWritten), bytesFree);
                qint64 written = m_audio.device->write((const char*)output + bytesWritten, toWrite);
                
                if (written > 0)
                {
                    bytesWritten += written;
                }
                else
                {
                    break;
                }
            }
        }

        av_freep(&output);
        av_frame_free(&frame);
    }
}

double FFmpegDecoderThread::getMasterClock()
{
    return m_audio.output ? m_audioClock : m_videoClock;
}

void FFmpegDecoderThread::updateVideoClock(double pts)
{
    m_videoClock = pts;
}

void FFmpegDecoderThread::updateAudioClock(double pts)
{
    m_audioClock = pts;
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
    m_pauseCondition.wakeAll();
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

// FFmpegPlayer 实现

FFmpegPlayer::FFmpegPlayer(QWidget *parent) : QWidget(parent),
                                              m_displayLabel_(new QLabel(this)),
                                              m_playerCore_(new PlayerWidgetBase(this)),
                                              m_decoder_(new FFmpegDecoderThread(this))
{
    m_displayLabel_->setAlignment(Qt::AlignCenter);
    m_displayLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_displayLabel_->setScaledContents(true);
    m_displayLabel_->setStyleSheet("background-color: black;");

    setupConnections();
}

FFmpegPlayer::~FFmpegPlayer()
{
    // 安全停止解码线程
    if (m_decoder_ && m_decoder_->isRunning())
    {
        disconnect(m_decoder_, nullptr, this, nullptr); // 断开所有连接
        m_decoder_->close(false);                       // 不等待线程结束
        m_decoder_->deleteLater();                      // 延迟删除
    }
}

PlayerWidgetBase *FFmpegPlayer::PlayerCore() const
{
    return m_playerCore_;
}

void FFmpegPlayer::setupConnections()
{
    // 所有连接使用QueuedConnection确保线程安全
    connect(m_decoder_, &FFmpegDecoderThread::frameReady, 
            this, &FFmpegPlayer::updateFrame, Qt::QueuedConnection);
    // 解码器 -> 播放器核心
    connect(m_decoder_, &FFmpegDecoderThread::positionChanged, 
            m_playerCore_, &PlayerWidgetBase::currentPosition, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::durationChanged, 
            m_playerCore_, &PlayerWidgetBase::currentDuration, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::playStateChanged, 
            m_playerCore_, &PlayerWidgetBase::playStateChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::volumeChanged, 
            m_playerCore_, &PlayerWidgetBase::currentVolume, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::muteStateChanged, 
            m_playerCore_, &PlayerWidgetBase::muteStateChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::errorOccurred, 
            m_playerCore_, &PlayerWidgetBase::errorInfoShow, Qt::QueuedConnection);
    // 播放器核心 -> 解码器
    connect(m_playerCore_, &PlayerWidgetBase::setPlayerFile, 
            this, &FFmpegPlayer::play, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::setPlayerUrl, 
            this, &FFmpegPlayer::play, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::changePlayState, 
            m_decoder_, &FFmpegDecoderThread::setPaused, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::changeMuteState, 
            m_decoder_, &FFmpegDecoderThread::setMute, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::seekPlay, 
            m_decoder_, &FFmpegDecoderThread::seekTo, Qt::QueuedConnection);
    connect(m_playerCore_, &PlayerWidgetBase::setVolume, 
            m_decoder_, &FFmpegDecoderThread::setVolume, Qt::QueuedConnection);
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
        // 安全停止解码线程
        m_decoder_->close(false); // 不等待线程结束

        // 使用单次连接确保线程结束后删除
        connect(m_decoder_, &QThread::finished,
                m_decoder_, &QObject::deleteLater, Qt::QueuedConnection);
    }
}

void FFmpegPlayer::updateFrame(const QImage &image)
{
    if (!image.isNull())
    {
        m_displayLabel_->setPixmap(QPixmap::fromImage(image)
                                       .scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

void FFmpegPlayer::resizeEvent(QResizeEvent *event)
{
    m_displayLabel_->resize(event->size());
    QWidget::resizeEvent(event);
}

#endif