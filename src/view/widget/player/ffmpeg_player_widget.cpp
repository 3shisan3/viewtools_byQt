#ifdef CAN_USE_FFMPEG

#include "ffmpeg_player_widget.h"

#include <QDebug>
#include <QResizeEvent>
#include <QDateTime>

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

FFmpegDecoderThread::FFmpegDecoderThread(QObject *parent) : QThread(parent)
{
    avformat_network_init();
    av_log_set_callback(ffmpegLogCallback);
    av_log_set_level(AV_LOG_DEBUG);
}

FFmpegDecoderThread::~FFmpegDecoderThread()
{
    close(false); // 避免等待自身线程
}

bool FFmpegDecoderThread::openMedia(const QString &url)
{
    close();

    QMutexLocker lock(&m_mutex);
    m_firstVideoPts = -1;
    m_videoStartTime = 0;

    // 打开输入流
    AVDictionary *options = nullptr;
    if (url.startsWith("rtsp://"))
    {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "5000000", 0);
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

    // 获取总时长
    qint64 duration = (m_formatCtx->duration != AV_NOPTS_VALUE) ? m_formatCtx->duration * 1000 / AV_TIME_BASE : 0;
    emit durationChanged(duration);

    m_clockTimer.start();
    m_running = true;
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

    // 初始化图像转换上下文
    m_video.swsCtx = sws_getContext(
        m_video.codecCtx->width, m_video.codecCtx->height, m_video.codecCtx->pix_fmt,
        m_video.codecCtx->width, m_video.codecCtx->height, AV_PIX_FMT_RGB32,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

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

    // 获取解码器参数
    AVCodecParameters *params = m_formatCtx->streams[m_audio.streamIndex]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(params->codec_id);
    if (!codec)
    {
        qWarning() << "Unsupported audio codec";
        return false;
    }

    // 初始化解码上下文
    m_audio.codecCtx = avcodec_alloc_context3(codec);
    if (!m_audio.codecCtx || avcodec_parameters_to_context(m_audio.codecCtx, params) < 0)
    {
        avcodec_free_context(&m_audio.codecCtx);
        return false;
    }

    // 处理声道布局
    if (params->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
    {
        av_channel_layout_default(&m_audio.codecCtx->ch_layout, params->ch_layout.nb_channels);
    }

    // 打开解码器
    if (avcodec_open2(m_audio.codecCtx, codec, nullptr) < 0)
    {
        avcodec_free_context(&m_audio.codecCtx);
        return false;
    }

    // 初始化重采样器
    m_audio.swrCtx = swr_alloc();
    av_opt_set_chlayout(m_audio.swrCtx, "in_chlayout", &m_audio.codecCtx->ch_layout, 0);
    av_opt_set_int(m_audio.swrCtx, "in_sample_rate", m_audio.codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "in_sample_fmt", m_audio.codecCtx->sample_fmt, 0);

    AVChannelLayout out_chlayout = AV_CHANNEL_LAYOUT_STEREO;
    av_opt_set_chlayout(m_audio.swrCtx, "out_chlayout", &out_chlayout, 0);
    av_opt_set_int(m_audio.swrCtx, "out_sample_rate", m_out_sample_rate, 0);
    av_opt_set_sample_fmt(m_audio.swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if (swr_init(m_audio.swrCtx) < 0)
    {
        swr_free(&m_audio.swrCtx);
        return false;
    }

    // 创建音频输出设备
    QAudioFormat format = createAudioFormat();
#if QT_VERSION_MAJOR < 6
    QAudioDeviceInfo deviceInfo(QAudioDeviceInfo::defaultOutputDevice());
    if (!deviceInfo.isFormatSupported(format))
    {
        format = deviceInfo.nearestFormat(format);
        m_out_sample_rate = format.sampleRate();
        qWarning() << "Using nearest audio format with sample rate:" << m_out_sample_rate;
    }
    m_audio.output = new QAudioOutput(deviceInfo, format, this);
#else
    QAudioDevice deviceInfo(QMediaDevices::defaultAudioOutput());
    if (!deviceInfo.isFormatSupported(format))
    {
        // Qt6中需要手动调整格式
        if (!format.isValid()) {
            format.setChannelCount(2);
            format.setSampleFormat(QAudioFormat::Int16);
            format.setSampleRate(m_out_sample_rate);
        }
        qWarning() << "Using adjusted audio format with sample rate:" << m_out_sample_rate;
    }
    m_audio.output = new QAudioSink(deviceInfo, format, this);
#endif

    m_audio.device = m_audio.output->start();

    if (!m_audio.device)
    {
        qWarning() << "Failed to start audio device";
        return false;
    }

    qDebug() << "Audio initialized successfully";
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
        wait();
    }

    // 安全释放音频资源
    if (m_audio.output)
    {
        // 先停止设备再释放资源
        m_audio.output->stop();

        // 延迟删除音频输出对象
        QMetaObject::invokeMethod(m_audio.output, "deleteLater", Qt::QueuedConnection);
        m_audio.output = nullptr;
        m_audio.device = nullptr;
    }

    // 释放视频资源
    if (m_video.swsCtx)
    {
        sws_freeContext(m_video.swsCtx);
        m_video.swsCtx = nullptr;
    }
    if (m_video.codecCtx)
    {
        avcodec_free_context(&m_video.codecCtx);
        m_video.codecCtx = nullptr;
    }

    // 释放音频资源
    if (m_audio.swrCtx)
    {
        swr_free(&m_audio.swrCtx);
        m_audio.swrCtx = nullptr;
    }
    if (m_audio.codecCtx)
    {
        avcodec_free_context(&m_audio.codecCtx);
        m_audio.codecCtx = nullptr;
    }

    if (m_formatCtx)
    {
        avformat_close_input(&m_formatCtx);
        avformat_free_context(m_formatCtx);
        m_formatCtx = nullptr;
    }

    emit playStateChanged(PlayerWidgetBase::StoppedState);
}

void FFmpegDecoderThread::run()
{
    AVPacket *pkt = av_packet_alloc();
    qint64 lastVideoTime = 0;
    qint64 frameDelay = 40000; // 默认40ms(25fps)

    // 正确计算视频帧间隔时间(微秒)
    if (m_video.codecCtx && m_video.streamIndex >= 0)
    {
        AVRational frameRate = av_guess_frame_rate(m_formatCtx,
                                                   m_formatCtx->streams[m_video.streamIndex],
                                                   nullptr);
        if (frameRate.den && frameRate.num)
        {
            // 修正帧间隔计算：从帧率转换为微秒间隔
            frameDelay = 1000000 * frameRate.den / frameRate.num;
            qDebug() << "Detected frame rate:" << frameRate.num << "/" << frameRate.den
                     << "Frame interval:" << frameDelay << "us";
        }
    }

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
            if (ret == AVERROR_EOF)
            {
                emit positionChanged(duration());
                break;
            }
            QThread::msleep(10);
            continue;
        }

        // 视频包处理
        if (pkt->stream_index == m_video.streamIndex)
        {
            qint64 currentTime = QDateTime::currentMSecsSinceEpoch() * 1000;

            // 控制视频帧显示间隔
            if (lastVideoTime > 0)
            {
                qint64 elapsed = currentTime - lastVideoTime;
                if (elapsed < frameDelay / 1000)
                { // 转换为毫秒比较
                    QThread::usleep((frameDelay / 1000) - elapsed);
                }
            }

            decodeVideoPacket(pkt);
            lastVideoTime = QDateTime::currentMSecsSinceEpoch() * 1000;
        }
        // 音频包处理
        else if (pkt->stream_index == m_audio.streamIndex)
        {
            decodeAudioPacket(pkt);
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    close(false);
}

void FFmpegDecoderThread::decodeVideoPacket(AVPacket *pkt)
{
    int ret = avcodec_send_packet(m_video.codecCtx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
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

        // 计算显示时间戳
        qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
        pts = av_rescale_q(pts, m_formatCtx->streams[m_video.streamIndex]->time_base, {1, 1000000});

        if (m_firstVideoPts == -1)
        {
            m_firstVideoPts = pts;
            m_videoStartTime = QDateTime::currentMSecsSinceEpoch() * 1000;
        }

        updateVideoClock(pts);
        displayVideoFrame(frame);
        av_frame_free(&frame);
    }
}

void FFmpegDecoderThread::displayVideoFrame(AVFrame *frame)
{
    // 计算显示时间戳（微秒）
    qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
    pts = av_rescale_q(pts,
                       m_formatCtx->streams[m_video.streamIndex]->time_base,
                       {1, 1000000});

    // 记录首帧PTS和系统时间
    if (m_firstVideoPts == -1)
    {
        m_firstVideoPts = pts;
        m_videoStartTime = QDateTime::currentMSecsSinceEpoch() * 1000;
        qDebug() << "First video frame PTS:" << m_firstVideoPts
                 << "Start time:" << m_videoStartTime;
    }

    // 计算应该显示的时间
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch() * 1000;
    qint64 expectedTime = m_videoStartTime + (pts - m_firstVideoPts);
    qint64 delay = expectedTime - currentTime;

    // 音视频同步
    if (m_audio.codecCtx)
    {
        // 有音频时使用音频时钟作为参考
        qint64 audioClock = m_audio.clock;
        qint64 audioDiff = pts - audioClock;

        // 优先使用音频同步
        if (abs(audioDiff) > 5000)
        { // 差异大于5ms时使用音频同步
            delay = audioDiff;
        }
    }

    // 控制显示时间（限制最大延迟为1秒）
    delay = qBound(-1000000LL, delay, 1000000LL);

    if (delay > 1000)
    { // 需要延迟
        QThread::usleep(delay);
    }
    else if (delay < -100000)
    { // 视频落后超过100ms则跳过
        qDebug() << "Dropping late frame, late by:" << -delay << "us";
        return;
    }

    updateVideoClock(pts);

    // 转换为RGB32
    uint8_t *data[1] = {new uint8_t[m_video.codecCtx->width * m_video.codecCtx->height * 4]};
    int linesize[1] = {m_video.codecCtx->width * 4};

    sws_scale(m_video.swsCtx, frame->data, frame->linesize, 0,
              frame->height, data, linesize);

    QImage image(data[0], m_video.codecCtx->width, m_video.codecCtx->height, QImage::Format_RGB32, [](void *ptr)
                 { delete[] static_cast<uint8_t *>(ptr); }, data[0]);

    emit frameReady(image.copy());
    emit positionChanged(pts / 1000); // 转为毫秒
}

void FFmpegDecoderThread::decodeAudioPacket(AVPacket *pkt)
{
    if (!m_audio.swrCtx || !m_audio.codecCtx || !m_audio.device)
    {
        return;
    }

    int ret = avcodec_send_packet(m_audio.codecCtx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        qWarning() << "Error sending audio packet:" << errbuf;
        return;
    }

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
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            qWarning() << "Error receiving audio frame:" << errbuf;
            av_frame_free(&frame);
            break;
        }

        // 计算PTS并立即更新音频时钟
        qint64 pts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
        pts = av_rescale_q(pts, m_formatCtx->streams[m_audio.streamIndex]->time_base, {1, 1000000});

        {
            QMutexLocker lock(&m_mutex);
            m_audio.clock = pts;
        }

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
            qWarning() << "Failed to allocate audio samples";
            av_frame_free(&frame);
            break;
        }

        // 执行重采样
        int realSamples = swr_convert(m_audio.swrCtx, &output, out_samples,
                                      (const uint8_t **)frame->data, frame->nb_samples);

        if (realSamples > 0)
        {
            QMutexLocker lock(&m_mutex);

            // 静音处理
            if (m_muted)
            {
                memset(output, 0, realSamples * 4); // 16-bit stereo = 4 bytes per sample
            }
            // 音量控制
            else if (m_volume < 0.99f || m_volume > 1.01f)
            {
                applyVolume(output, realSamples * 4);
            }

            // 分块写入音频数据
            qint64 bytesToWrite = realSamples * 4;
            qint64 bytesWritten = 0;
            const uint8_t *dataPtr = output;
            int retryCount = 3;

            while (bytesWritten < bytesToWrite && retryCount-- > 0)
            {
                qint64 freeBytes = static_cast<qint64>(m_audio.output->bytesFree());
                qint64 remainingBytes = bytesToWrite - bytesWritten;
                qint64 chunkSize = qMin(remainingBytes, freeBytes);

                if (chunkSize <= 0)
                {
                    QThread::usleep(1000);
                    continue;
                }

                qint64 written = m_audio.device->write(
                    reinterpret_cast<const char *>(dataPtr + bytesWritten),
                    static_cast<qint64>(chunkSize));

                if (written > 0)
                {
                    bytesWritten += written;
                }
                else
                {
                    break;
                }
            }

            if (bytesWritten < bytesToWrite)
            {
                qDebug() << "Audio data dropped:" << (bytesToWrite - bytesWritten) << "bytes";
            }
        }

        av_freep(&output);
        av_frame_free(&frame);
    }
}

void FFmpegDecoderThread::applyVolume(uint8_t *data, int len)
{
    int16_t *samples = reinterpret_cast<int16_t *>(data);
    const int count = len / sizeof(int16_t);

    for (int i = 0; i < count; ++i)
    {
        samples[i] = static_cast<int16_t>(qBound(-32768.0f, samples[i] * m_volume, 32767.0f));
    }
}

qint64 FFmpegDecoderThread::getMasterClock() const
{
    return m_audio.codecCtx ? m_audio.clock : m_video.clock;
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

    // 立即应用静音状态
    if (m_audio.device && m_muted)
    {
#if QT_VERSION_MAJOR < 6
        m_audio.device->write(QByteArray(m_audio.output->periodSize(), 0));
#else
        // Qt6 使用 bufferSize() 替代 periodSize()
        m_audio.device->write(QByteArray(m_audio.output->bufferSize(), 0));
#endif
    }

    emit muteStateChanged(m_muted);
}

FFmpegPlayer::FFmpegPlayer(QWidget *parent) : QWidget(parent),
                                              m_displayLabel_(new QLabel(this)),
                                              m_playerCore_(new PlayerWidgetBase(this)),
                                              m_decoder_(new FFmpegDecoderThread(this))
{

    m_displayLabel_->setAlignment(Qt::AlignCenter);
    m_displayLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_displayLabel_->setAttribute(Qt::WA_OpaquePaintEvent);

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

PlayerWidgetBase *FFmpegPlayer::playerCore() const
{
    return m_playerCore_;
}

void FFmpegPlayer::setupConnections()
{
    // 所有连接使用QueuedConnection确保线程安全
    connect(m_decoder_, &FFmpegDecoderThread::frameReady, this, &FFmpegPlayer::updateFrame, Qt::QueuedConnection);
    // 解码器 -> 播放器核心
    connect(m_decoder_, &FFmpegDecoderThread::positionChanged, m_playerCore_, &PlayerWidgetBase::currentPosition, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::durationChanged, m_playerCore_, &PlayerWidgetBase::currentDuration, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::playStateChanged, m_playerCore_, &PlayerWidgetBase::playStateChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::volumeChanged, m_playerCore_, &PlayerWidgetBase::currentVolume, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::muteStateChanged, m_playerCore_, &PlayerWidgetBase::muteStateChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::errorOccurred, m_playerCore_, &PlayerWidgetBase::errorInfoShow, Qt::QueuedConnection);
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
        // 安全停止解码线程
        m_decoder_->close(false); // 不等待线程结束

        // 使用单次连接确保线程结束后删除
        connect(m_decoder_, &QThread::finished,
                m_decoder_, &QObject::deleteLater, Qt::QueuedConnection);
    }
}

void FFmpegPlayer::updateFrame(const QImage &image)
{
    m_displayLabel_->setPixmap(QPixmap::fromImage(image)
                                   .scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void FFmpegPlayer::resizeEvent(QResizeEvent *event)
{
    m_displayLabel_->resize(event->size());
    QWidget::resizeEvent(event);
}

#endif