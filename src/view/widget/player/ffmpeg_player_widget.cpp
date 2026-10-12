#ifdef CAN_USE_FFMPEG

#include "ffmpeg_player_widget.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QHideEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <limits>

// ==================== 4K视频优化配置常量 ====================
#define _4K_WIDTH_THRESHOLD   3840
#define _4K_HEIGHT_THRESHOLD  2160
#define _4K_BUFFER_SIZE       4          // 4K缓冲区大小（帧）
#define _1080P_BUFFER_SIZE    8          // 1080p缓冲区大小
#define _LOW_RES_BUFFER_SIZE  12         // 低分辨率缓冲区大小

// 4K优化：最大等待时间系数
#define _4K_WAIT_TIME_FACTOR  0.5f

// ==================== 时间戳归一化参数 ====================
// 单个包允许的最大时间跨度：正常帧间隔约 40ms，超过 1 秒即视为时间戳跳变
static const qint64 kTsMaxStepUs = 1000000;
// 无正常步长可参考时的兜底步长
static const qint64 kTsFallbackStepUs = 40000;
// 判定"正常步长"的下限：同一条流相邻两个包的时间差不可能小于 1 毫秒，
// 更小的值属于坏包/重复包，不能拿来当典型帧间隔
static const qint64 kTsMinStepUs = 1000;
// 假步长（B 帧重排造成的 3~4 倍步长）判定阈值：达到典型值的 3 倍即视为异常
static const qint64 kTsTypicalStepMul = 3;

// ==================== 时间轴归一化 ====================

void TimestampNormalizer::reset()
{
    anchors.clear();
    m_hasOrigin = false;
    m_originRawUs = 0;
}

qint64 TimestampNormalizer::normalize(qint64 rawUs, Clock &clock)
{
    if (rawUs == AV_NOPTS_VALUE)
        return clock.lastOutUs;     // 无时间戳：沿用上一个，避免时间轴倒退

    if (!clock.anchored)
    {
        // 该流的第一帧（或 seek 后的第一帧）：确定归一化起点
        clock.anchored = true;
        clock.lastRawUs = rawUs;
        clock.lastStepUs = 0;

        if (clock.pendingOutUs >= 0)
        {
            // seek 后：按"目标位置 + 该帧相对目标源时间戳的偏移"锚定。
            // 两条流用同一套换算，彼此之间原有的相对偏移（B 帧重排等）不会被抹平
            clock.lastOutUs = clock.pendingOutUs + (rawUs - clock.pendingRawUs);
        }
        else if (clock.seekTargetUs >= 0)
        {
            // 按字节位置 seek 后落点未知：源时间戳在这两个 VOB 里会重复出现
            // （时间轴中途归零/回绕），这里取"最接近目标"的那个锚点候选，
            // 否则会把第二趟的画面当成第一趟的位置来显示
            clock.lastOutUs = resolveNearest(rawUs, clock.seekTargetUs);
        }
        else
        {
            // 整条时间轴的共同起点取所有流里最早的源时间戳，
            // 音视频之间的相对偏移（B 帧重排延迟）因此得以保留
            if (!m_hasOrigin)
            {
                m_originRawUs = rawUs;
                m_hasOrigin = true;
            }
            clock.lastOutUs = rawUs - m_originRawUs;
            if (clock.lastOutUs < 0)
                clock.lastOutUs = 0;
        }
        clock.pendingOutUs = -1;
        clock.seekTargetUs = -1;
        anchors.append(qMakePair(clock.lastOutUs, rawUs));
        return clock.lastOutUs;
    }

    qint64 step = rawUs - clock.lastRawUs;
    if (step <= 0 || step > kTsMaxStepUs)
    {
        // 源时间戳跳变（归零重来 / 33 位回绕 / 坏包 / B 帧重排回退）：不跟随跳变，
        // 按正常步长继续推进时间轴，并记录锚点供 seek 反算
        step = (clock.lastStepUs > 0) ? clock.lastStepUs : kTsFallbackStepUs;
        anchors.append(qMakePair(clock.lastOutUs + step, rawUs));
    }
    else if (clock.lastStepUs <= 0)
    {
        // 第一个正常步长：确定典型帧间隔
        clock.lastStepUs = step;
    }
    else if (step >= clock.lastStepUs * kTsTypicalStepMul)
    {
        // 步长达到典型值的 3 倍以上：同样是假步长，按正常步长推进。
        // 典型来源是 B 帧重排 —— 封装把 dts 存成 pts-重排量之后，
        // dts 序列会出现 "-80ms / +160ms" 这种成对的抖动（实测转码 MKV：
        // 107963 个包里 26992 个回退），若把 +160ms 当正常步长累加，
        // 每次回退还要再补一个偏大的兜底步长，总时长会虚高到 1.75 倍
        step = clock.lastStepUs;
    }
    else if (step >= kTsMinStepUs && step < clock.lastStepUs)
    {
        // 典型步长取"出现过的最小正常步长"。
        // 不能直接沿用"上一个步长"：一旦上面那种 3~4 倍的假步长被记下来，
        // 之后每次跳变都会按它累加。加 1ms 下限，避免单个异常小步长把典型步长压到接近 0。
        clock.lastStepUs = step;
    }

    clock.lastRawUs = rawUs;
    clock.lastOutUs += step;
    return clock.lastOutUs;
}

qint64 TimestampNormalizer::toSource(qint64 normalizedUs) const
{
    // 锚点表在播放过程中会被追加（每遇到一次时间戳跳变加一个），
    // 音视频两条流各自追加的顺序不保证按归一化时间递增，
    // 所以这里扫完整张表取"归一化时间不超过目标、且最大"的那个锚点，
    // 不能一遇到更大的就 break，否则会取到偏早的锚点、反算出的源时间偏小。
    qint64 anchorNorm = 0;
    qint64 anchorRaw = 0;
    bool found = false;
    for (const QPair<qint64, qint64> &anchor : anchors)
    {
        if (normalizedUs >= anchor.first && (!found || anchor.first >= anchorNorm))
        {
            anchorNorm = anchor.first;
            anchorRaw = anchor.second;
            found = true;
        }
    }
    if (!found && !anchors.isEmpty())
    {
        // 目标在第一个锚点之前：退到最早的一个锚点
        anchorNorm = anchors.at(0).first;
        anchorRaw = anchors.at(0).second;
    }
    return anchorRaw + (normalizedUs - anchorNorm);
}

qint64 TimestampNormalizer::resolveNearest(qint64 rawUs, qint64 targetUs) const
{
    if (anchors.isEmpty())
        return rawUs;

    // 同一个源时间戳可能对应时间轴上多个位置（中途归零/回绕），
    // 每个锚点都能给出一个候选，取最接近 seek 目标的那一个
    qint64 best = rawUs;
    qint64 bestErr = -1;
    for (const QPair<qint64, qint64> &anchor : anchors)
    {
        const qint64 cand = anchor.first + (rawUs - anchor.second);
        const qint64 err = qAbs(cand - targetUs);
        if (bestErr < 0 || err < bestErr)
        {
            bestErr = err;
            best = cand;
        }
    }
    return best;
}

void TimestampNormalizer::mergeAnchors(const QVector<QPair<qint64, qint64>> &other)
{
    // 并入后台扫描得到的锚点（时间戳跳变点）。
    // 锚点是文件的固有结构：源时间戳在哪里归零/回绕，与播放到过哪里无关。
    // 只靠播放过程积累的话，用户直接拖到后半段时跳变点还没被"踩"到，
    // toSource/resolveNearest 就会把第二趟的画面当成第一趟的位置。
    //
    // 这里直接追加、不做去重：重复的锚点对 toSource/resolveNearest 没有影响
    // （只是多扫几项），而去重要两层循环比对，锚点动辄上万条时会卡住整个 seek。
    // 调用方用"扫描结果条数"做守卫，保证同一份结果只并一次。
    anchors += other;
}

void TimestampNormalizer::rebase(qint64 normalizedUs, Clock &c1, Clock &c2, bool landingKnown)
{
    // 注意：这里【不裁剪】锚点表。锚点记录的是"源时间戳在哪几个位置发生过跳变"，
    // 是整条时间轴的结构信息，播放到第二趟时还要靠它把归一化时间反算回源时间。
    // 之前按目标位置裁剪，导致 seek 一次之后 toSource 退化成线性外推，
    // 再次 seek 到后半段就会算出越界的源时间戳（seek 直接跳到文件尾）。
    c1 = Clock();
    c2 = Clock();

    if (landingKnown)
    {
        // 按时间戳 seek：确定落点就是 toSource(normalizedUs) 对应的位置
        const qint64 sourceUs = toSource(normalizedUs);
        c1.pendingOutUs = c2.pendingOutUs = normalizedUs;
        c1.pendingRawUs = c2.pendingRawUs = sourceUs;
    }
    else
    {
        // 按字节位置 seek：落点未知，第一帧用"最接近目标的锚点"反算归一化时间
        c1.seekTargetUs = c2.seekTargetUs = normalizedUs;
    }
}

// ==================== 内容时长扫描 ====================
MediaDurationScanner::MediaDurationScanner(QObject *parent)
    : QThread(parent)
{
}

MediaDurationScanner::~MediaDurationScanner()
{
    cancel();
    if (isRunning())
        wait(3000);
}

void MediaDurationScanner::cancel()
{
    m_cancel.store(true);
}

void MediaDurationScanner::startScan(const QString &url)
{
    cancel();
    if (isRunning())
        wait(3000);

    m_cancel.store(false);
    m_url = url;
    start();
}

void MediaDurationScanner::run()
{
    const QString url = m_url;
    {
        QMutexLocker lock(&m_anchorMutex);
        m_anchors.clear();
    }

    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, url.toUtf8().constData(), nullptr, nullptr) < 0)
    {
        emit durationScanned(url, 0);
        return;
    }
    if (avformat_find_stream_info(ctx, nullptr) < 0)
    {
        avformat_close_input(&ctx);
        emit durationScanned(url, 0);
        return;
    }

    const int vIdx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int aIdx = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (vIdx < 0 && aIdx < 0)
    {
        avformat_close_input(&ctx);
        emit durationScanned(url, 0);
        return;
    }

    // 与播放时同一套归一化规则：保证"进度条总长"和"播放到的位置"一致
    TimestampNormalizer normalizer;
    TimestampNormalizer::Clock clocks[2];
    qint64 maxOut = 0;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        avformat_close_input(&ctx);
        emit durationScanned(url, 0);
        return;
    }

    while (!m_cancel.load() && av_read_frame(ctx, pkt) >= 0)
    {
        const int slot = (pkt->stream_index == vIdx) ? 0 : ((pkt->stream_index == aIdx) ? 1 : -1);
        if (slot >= 0)
        {
            int64_t raw = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
            if (raw != AV_NOPTS_VALUE)
            {
                raw = av_rescale_q(raw, ctx->streams[pkt->stream_index]->time_base, AV_TIME_BASE_Q);
                const qint64 out = normalizer.normalize(raw, clocks[slot]);
                if (out > maxOut)
                    maxOut = out;
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&ctx);

    // 记录这次扫描发现的时间戳跳变点，供播放端反算使用
    {
        QMutexLocker lock(&m_anchorMutex);
        m_anchors = normalizer.anchors;
    }

    if (m_cancel.load())
        return;     // 已被取消：不发结果，避免旧文件的时长覆盖新文件

    emit durationScanned(url, maxOut / 1000);   // 微秒 -> 毫秒
}

QVector<QPair<qint64, qint64>> MediaDurationScanner::scannedAnchors() const
{
    QMutexLocker lock(&m_anchorMutex);
    return m_anchors;
}

/**
 * @brief 检查是否为4K视频
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
    , m_pauseStartTime(0)
    , m_totalPausedTime(0)
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

    qDebug() << "FFmpegDecoderThread initialized";
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

// ==================== 时间戳归一化 ====================

void FFmpegDecoderThread::resetTimestampClocks()
{
    m_tsNorm.reset();
    m_videoClock = TimestampNormalizer::Clock();
    m_audioClock = TimestampNormalizer::Clock();
}

qint64 FFmpegDecoderThread::normalizeTimestamp(qint64 rawUs, TimestampNormalizer::Clock &clock)
{
    return m_tsNorm.normalize(rawUs, clock);
}

qint64 FFmpegDecoderThread::toSourceTimestamp(qint64 normalizedUs) const
{
    return m_tsNorm.toSource(normalizedUs);
}

void FFmpegDecoderThread::rebaseTimestampClocks(qint64 normalizedUs, bool landingKnown)
{
    m_tsNorm.rebase(normalizedUs, m_videoClock, m_audioClock, landingKnown);
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
    m_durationMs = 0;
    m_scannedDurationMs = 0;
    m_lastVideoPts = 0;
    m_firstVideoPts = -1;
    m_startTime = 0;
    m_playbackRate = 1.0f;      // 重置倍速
    m_needReinitAudio = false;
    m_seekTargetPts = -1;;      // 重置seek目标
    m_seekDecodingActive = false;   // 清掉上一次 seek 遗留的帧过滤状态
    m_seekFallbackUsed = false;
    m_mergedScanAnchorCount = -1;
    m_pauseStartTime = 0;
    m_totalPausedTime = 0;
    m_wasPaused = false;

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
        cleanupResources();
        return false;
    }

    // 初始化视频和音频解码器
    bool videoOk = initVideo();
    bool audioOk = initAudio();

    if (!videoOk && !audioOk)
    {
        emit errorOccurred(tr("No valid streams"));
        cleanupResources();
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
    m_durationMs = (m_formatCtx->duration != AV_NOPTS_VALUE)
                       ? m_formatCtx->duration * 1000 / AV_TIME_BASE : 0;
    resetTimestampClocks();

    // 切换/打开媒体时立刻复位进度显示，避免沿用上一个媒体的总时长与位置
    emit positionChanged(0);
    emit durationChanged(m_durationMs);

    // 后台扫描真实内容时长：容器 duration 对"时间戳中途归零"的 VOB 会少算 2 倍以上
    if (!isNetwork)
    {
        if (!m_durationScanner)
        {
            m_durationScanner = new MediaDurationScanner(this);
            connect(m_durationScanner, &MediaDurationScanner::durationScanned, this,
                    [this](const QString &scannedUrl, qint64 durationMs)
                    {
                        if (scannedUrl != m_currentUrl || durationMs <= 0)
                            return;     // 已切到别的媒体，或没扫出来
                        // 记录真实内容时长：按字节位置 seek 需要用它估算文件内的相对位置
                        m_scannedDurationMs = durationMs;
                        if (durationMs > m_durationMs)
                        {
                            m_durationMs = durationMs;
                            emit durationChanged(m_durationMs);
                        }
                    });
        }
        m_durationScanner->startScan(url);
    }

    // 启动音频缓冲区定时器
    if (m_audioBufferTimer)
        m_audioBufferTimer->start();

    // 启动解码线程
    m_running = true;
    emit playStateChanged(PlayerWidgetBase::PlayingState);
    start();

    qDebug() << "Media opened - Video:" << videoOk << "Audio:" << m_hasAudio
             << "Duration:" << m_durationMs << "ms";
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

    // 4K视频缓冲区增加到4帧，避免死锁
    if (m_video.width >= _4K_WIDTH_THRESHOLD || m_video.height >= _4K_HEIGHT_THRESHOLD)
    {
        bufferSize = _4K_BUFFER_SIZE;
        qDebug() << "4K video detected - using buffer:" << bufferSize << "frames";

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

    // 获取像素宽高比（SAR）：DVD/标清电视源（720x480、720x576）通常为非 1:1，
    // 需结合 SAR 计算出真实显示比例（4:3 或 16:9），否则画面会被拉变形。
    AVStream *videoStream = m_formatCtx->streams[m_video.streamIndex];
    m_video.sampleAspect = av_guess_sample_aspect_ratio(m_formatCtx, videoStream, nullptr);
    if (m_video.sampleAspect.num <= 0 || m_video.sampleAspect.den <= 0)
        m_video.sampleAspect = params->sample_aspect_ratio;
    if (m_video.sampleAspect.num <= 0 || m_video.sampleAspect.den <= 0)
        m_video.sampleAspect = AVRational{1, 1};

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

    // 通知上层视频画面几何信息（含像素宽高比），用于按正确显示比例渲染/适配窗口
    emit videoGeometryChanged(m_video.width, m_video.height,
                              m_video.sampleAspect.num, m_video.sampleAspect.den);

    qDebug() << "Video initialized successfully";
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
            qWarning() << "Decoder thread did not finish in time";
            return;
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
        delete m_audio.output;
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
    m_seekTargetPts = -1;
    m_pauseStartTime = 0;
    m_totalPausedTime = 0;

    // 清空帧缓冲区
    m_frameBuffer.clear();

    qDebug() << "Resources cleaned up";
}

// ==================== 精确Seek实现 ====================

/**
 * @brief 按字节位置跳转
 *
 * 这两个 VOB 的源时间戳非单调（VTS_01_1 时间轴中途归零、VTS_01_2 出现一次 33 位回绕），
 * 同一个时间戳在文件里会出现多次，而且解复用器索引里存在"字节位置在文件末尾、
 * 时间戳却落在中段"的条目（探测尾部时写入的）。结果是 av_seek_frame(时间戳) 有两个坑：
 *   1. 目标时间戳超过索引里最大的时间戳时，FFmpeg 会跳到最后一个索引条目的字节位置
 *      再往后读 —— 那个位置就是文件尾，于是立刻读到 EOF；
 *   2. 即使定位成功，同一个时间戳也可能对应第一趟的画面而不是目标所在的第二趟。
 * 内容在文件里的存储顺序才是播放顺序，所以这里按字节位置跳：
 *   先按"内容占比"估算字节位置，读一个包反算落点，落点偏后就按比例回退重试。
 *
 * @param targetPts 目标位置（微秒，归一化时间轴）
 * @return true 表示已按字节定位完成；false 表示不适用/失败，调用方应退回时间戳 seek
 */
bool FFmpegDecoderThread::seekByBytePosition(qint64 targetPts)
{
    if (!m_formatCtx || !m_formatCtx->pb || m_video.streamIndex < 0)
        return false;
    // 只对本地文件按字节跳：网络流没有稳定的"文件大小"概念
    if (m_currentUrl.startsWith("rtsp://") || m_currentUrl.startsWith("http://"))
        return false;

    const qint64 fileSize = avio_size(m_formatCtx->pb);
    // 真实内容时长（后台扫描得到）。容器时长对"时间戳中途归零"的 VOB 会少算一倍以上，
    // 用它估算会让字节位置严重偏后；没扫出来之前不做字节 seek，交给时间戳 seek + 顺序读兜底。
    if (m_scannedDurationMs <= 0)
        return false;
    const qint64 totalUs = m_scannedDurationMs * 1000;
    if (fileSize <= 0 || targetPts <= 0)
        return false;

    qint64 bytePos = static_cast<qint64>(static_cast<double>(fileSize) *
                                         static_cast<double>(targetPts) / static_cast<double>(totalUs));
    bytePos = qBound<qint64>(0, bytePos, fileSize - 1);

    // 落点可接受的窗口
    const qint64 maxAheadUs = 1000000;                                  // 最多允许超前 1 秒
    const qint64 refineBehindUs = 2000000;                              // 落后 2 秒以上就修正估算
    const qint64 acceptBehindUs = qMax<qint64>(5000000, totalUs / 100); // 兜底可接受范围（约总长的 1%）

    bool converged = false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        // 部分解复用器不支持按字节定位（AVFMT_NO_BYTE_SEEK），此时返回 AVERROR(ENOSYS)，
        // 位置不会变，必须退回按时间戳 seek，否则会拿旧位置当落点
        if (av_seek_frame(m_formatCtx, -1, bytePos, AVSEEK_FLAG_BYTE) < 0)
            return false;
        if (m_video.codecCtx)
            avcodec_flush_buffers(m_video.codecCtx);
        if (m_audio.codecCtx)
            avcodec_flush_buffers(m_audio.codecCtx);

        // 探测落点：读第一个视频包，用锚点表反算它落在时间轴的哪个时刻
        qint64 landedUs = -1;
        AVPacket *probe = av_packet_alloc();
        if (!probe)
            break;
        while (av_read_frame(m_formatCtx, probe) >= 0)
        {
            if (probe->stream_index == m_video.streamIndex)
            {
                int64_t raw = (probe->dts != AV_NOPTS_VALUE) ? probe->dts : probe->pts;
                if (raw != AV_NOPTS_VALUE)
                {
                    raw = av_rescale_q(raw, m_video.timeBase, AV_TIME_BASE_Q);
                    landedUs = m_tsNorm.resolveNearest(raw, targetPts);
                    break;
                }
            }
            av_packet_unref(probe);
        }
        av_packet_free(&probe);

        if (landedUs < 0)
        {
            // 读不到包：估算位置多半已经落到文件末尾，往回退再试
            bytePos = bytePos * 9 / 10;
            continue;
        }

        const qint64 err = landedUs - targetPts;
        if (err <= maxAheadUs && err >= -refineBehindUs)
        {
            converged = true;   // 落点够准，直接用
            break;
        }

        // 按比例修正估算位置（按字节/内容大致成正比）
        const qint64 newPos = static_cast<qint64>(static_cast<double>(bytePos) *
                                                  static_cast<double>(targetPts) /
                                                  static_cast<double>(landedUs));
        const qint64 clamped = qBound<qint64>(0, newPos, fileSize - 1);
        if (clamped == bytePos)
        {
            // 位置已经推到头、修正不动了。这种情况说明"按字节定位"对这个封装不成立
            // （实测 Matroska：AVSEEK_FLAG_BYTE 返回成功但解复用器位置没动，
            //  读到的还是旧位置的包，于是估算被一路推到文件尾）。
            // 落点还不算太离谱就勉强采用，否则放弃，交给按时间戳 seek。
            converged = (err <= maxAheadUs && err >= -acceptBehindUs);
            break;
        }
        bytePos = clamped;
    }

    if (!converged)
        return false;

    // 用最终的字节位置真正定位一次（上面探测读掉的包不作数）
    if (av_seek_frame(m_formatCtx, -1, bytePos, AVSEEK_FLAG_BYTE) < 0)
        return false;

    return true;
}

/**
 * @brief 兜底跳转：从头顺序读到目标位置
 *
 * 按字节跳也失败时使用。源时间戳非单调的文件里，顺序读是唯一一定正确的定位方式。
 * 归一化是确定性的，重新跑一遍就能得到与进度条一致的时间轴。
 * 过程中只推进时间轴、不解码，到目标前 2 秒再交回正常解码流程，避免白白解码几十万帧。
 *
 * @return true 表示已推进到目标附近
 */
bool FFmpegDecoderThread::performSequentialSeek(qint64 targetMs)
{
    if (!m_formatCtx || m_video.streamIndex < 0)
        return false;

    // 回到文件开头并复位归一化状态
    if (av_seek_frame(m_formatCtx, -1, 0, AVSEEK_FLAG_BACKWARD) < 0 && m_formatCtx->pb)
        avio_seek(m_formatCtx->pb, 0, SEEK_SET);
    resetTimestampClocks();
    if (m_video.codecCtx)
        avcodec_flush_buffers(m_video.codecCtx);
    if (m_audio.codecCtx)
        avcodec_flush_buffers(m_audio.codecCtx);
    m_frameBuffer.clear();

    const qint64 targetPts = targetMs * 1000;
    const qint64 stopAheadUs = 2000000;     // 到目标前 2 秒就交回正常流程

    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return false;

    bool reached = false;
    while (m_running && av_read_frame(m_formatCtx, pkt) >= 0)
    {
        const int idx = pkt->stream_index;
        if (idx == m_video.streamIndex || (idx == m_audio.streamIndex && m_hasAudio))
        {
            int64_t raw = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
            if (raw != AV_NOPTS_VALUE)
            {
                const bool isVideo = (idx == m_video.streamIndex);
                raw = av_rescale_q(raw, isVideo ? m_video.timeBase : m_audio.timeBase, AV_TIME_BASE_Q);
                const qint64 out = isVideo ? normalizeTimestamp(raw, m_videoClock)
                                           : normalizeTimestamp(raw, m_audioClock);
                if (isVideo && out + stopAheadUs >= targetPts)
                {
                    reached = true;
                    av_packet_unref(pkt);
                    break;
                }
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return reached;
}

/**
 * @brief 执行精确跳转
 * @param targetMs 目标位置（毫秒）
 *
 * 定位策略：
 * 1. 优先按字节位置跳（源时间戳非单调时唯一可靠的定位方式，见 seekByBytePosition）
 * 2. 不适用（网络流等）时退回按时间戳跳最近的关键帧
 * 3. 之后解码直到达到目标PTS，丢弃中间帧；只有达到目标帧后才显示
 * 4. 若还没到目标就读到 EOF，说明定位失败，由 run() 触发 performSequentialSeek 兜底
 */
void FFmpegDecoderThread::performPreciseSeek(qint64 targetMs)
{
    if (!m_formatCtx || m_video.streamIndex < 0)
        return;

    qDebug() << "Performing precise seek to:" << targetMs << "ms";

    // 计算目标PTS（微秒，归一化时间轴）
    qint64 targetPts = targetMs * 1000;
    m_seekTargetPts = targetPts;

    // 把后台扫描发现的时间戳跳变点并入锚点表：这些跳变点是文件的固有结构，
    // 但只靠播放过程积累时，用户没"播放经过"跳变点就拿不到后半段的时间轴信息。
    // 必须在下面反算源时间之前做，否则 toSource/resolveNearest 用的是残缺的表。
    // 用"扫描结果条数"做守卫：同一份扫描结果只并一次，避免每次 seek 都追加一遍。
    if (m_durationScanner)
    {
        const QVector<QPair<qint64, qint64>> scanned = m_durationScanner->scannedAnchors();
        if (!scanned.isEmpty() && scanned.size() != m_mergedScanAnchorCount)
        {
            m_tsNorm.mergeAnchors(scanned);
            m_mergedScanAnchorCount = scanned.size();
        }
    }

    // 归一化时间 -> 源时间：源时间戳可能中途归零重来，直接按归一化值 seek 会定位不到
    const qint64 sourceUs = toSourceTimestamp(targetPts);

    // 转换到流时间基
    int64_t seekTarget = av_rescale_q(sourceUs, AV_TIME_BASE_Q,
                                       m_formatCtx->streams[m_video.streamIndex]->time_base);

    // 优先按字节位置跳
    const bool byByte = seekByBytePosition(targetPts);

    if (!byByte)
    {
        // 退回按时间戳跳转到最近的关键帧
        int ret = av_seek_frame(m_formatCtx, m_video.streamIndex,
                                seekTarget, AVSEEK_FLAG_BACKWARD);

        if (ret < 0)
        {
            qWarning() << "av_seek_frame failed, trying default stream";
            // 尝试默认流
            av_seek_frame(m_formatCtx, -1, sourceUs, AVSEEK_FLAG_BACKWARD);
        }
    }

    // seek 后重建归一化状态，使后续解码帧落在正确的时间段上。
    // 按字节跳时落点未知，交给第一帧用锚点表反算
    rebaseTimestampClocks(targetPts, !byByte);

    // 清空解码器缓冲区
    if (m_video.codecCtx)
    {
        avcodec_flush_buffers(m_video.codecCtx);
    }
    if (m_audio.codecCtx)
    {
        avcodec_flush_buffers(m_audio.codecCtx);
    }

    // 清空帧缓冲区
    m_frameBuffer.clear();

    // 重置PTS跟踪
    m_lastVideoPts = 0;
    m_seekDecodingActive = true;
    m_seekFallbackUsed = false;     // 每次 seek 只允许一次顺序读兜底，避免反复重读整个文件

    qDebug() << "Seek completed, will decode until target PTS:" << targetPts;
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
    bool eofReached = false;    // 是否自然播放到文件末尾（EOF）

    // 无音频模式的时间基准
    qint64 systemStartTime = 0;
    qint64 firstPts = -1;

    while (m_running)
    {
        // 空指针检查
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

        // ========== 暂停处理逻辑 ==========
        {
            QMutexLocker lock(&m_mutex);
            
            // 检测暂停状态变化
            if (m_paused && !m_wasPaused)
            {
                // 刚进入暂停状态，记录暂停开始时间
                m_pauseStartTime = av_gettime_relative();
                m_wasPaused = true;
                qDebug() << "Entered paused state at" << m_pauseStartTime;
            }
            else if (!m_paused && m_wasPaused)
            {
                // 刚退出暂停状态，计算总暂停时间
                if (m_pauseStartTime > 0)
                {
                    qint64 pauseDuration = av_gettime_relative() - m_pauseStartTime;
                    m_totalPausedTime += pauseDuration;
                    qDebug() << "Exited paused state, duration:" << pauseDuration 
                             << "us, total:" << m_totalPausedTime << "us";
                }
                m_pauseStartTime = 0;
                m_wasPaused = false;
                // 重置显示时间，避免时间累积问题
                m_lastDisplayTime = 0;
            }
            
            while (m_paused && m_running && !m_seekRequested)
            {
                // 使用较短的超时，确保能及时响应状态变化
                if (!m_pauseCondition.wait(&m_mutex, 50))
                {
                    // 超时后继续循环，检查状态
                }
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
            qint64 seekTargetMs = m_seekPos;
            m_seekRequested = false;
            m_seekPos = 0;
            
            // 执行精确seek
            performPreciseSeek(seekTargetMs);
            
            // 重置同步状态
            m_syncManager.reset();
            m_lastVideoPts = 0;
            firstPts = -1;
            systemStartTime = 0;
            // seek后重置暂停时间累积
            m_totalPausedTime = 0;
            
            // 发送位置更新信号
            emit positionChanged(seekTargetMs);
        }

        // 检查缓冲区是否过满
        bool is4K = (m_video.width >= _4K_WIDTH_THRESHOLD || m_video.height >= _4K_HEIGHT_THRESHOLD);
        int maxBufferSize = is4K ? (_4K_BUFFER_SIZE + 2) : 8;
        
        // ========== 缓冲区检查 ==========
        if (m_useBuffer && !m_paused && m_frameBuffer.size() >= maxBufferSize)
        {
            QThread::msleep(is4K ? 3 : 2);
            continue;
        }

        // 读取数据包
        av_packet_unref(pkt);
        int ret = av_read_frame(m_formatCtx, pkt);

        if (ret < 0)
        {
            if (ret == AVERROR_EOF)
            {
                // seek 之后还没到达目标就读到文件末尾：说明这次定位失败了
                // （源时间戳非单调时 av_seek_frame 会直接跳到文件尾）。
                // 这种情况绝不能当成"播放结束"，否则会误触发自动连播、把用户甩到下一段。
                if (m_seekDecodingActive && m_seekTargetPts > 0 && !m_seekFallbackUsed)
                {
                    const qint64 pendingTargetMs = m_seekTargetPts / 1000;
                    qWarning() << "Seek did not reach target before EOF, retrying by sequential scan:"
                               << pendingTargetMs << "ms";
                    m_seekFallbackUsed = true;
                    m_seekDecodingActive = false;

                    if (performSequentialSeek(pendingTargetMs))
                    {
                        // 已推进到目标附近，交回正常解码流程继续丢弃到目标帧
                        m_seekTargetPts = pendingTargetMs * 1000;
                        m_seekDecodingActive = true;
                        m_lastVideoPts = 0;
                        firstPts = -1;
                        systemStartTime = 0;
                        m_syncManager.reset();
                        continue;
                    }
                }

                // EOF 后等待一段时间再退出
                if (m_frameBuffer.size() > 0)
                {
                    QThread::msleep(10);
                    continue;
                }
                eofReached = true; // 自然播放到文件末尾
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

        if (m_useBuffer && !m_paused)
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
                        systemStartTime = av_gettime_relative() - m_totalPausedTime;
                        waitTime = 0;
                    }
                    else
                    {
                        // 考虑暂停时间
                        qint64 expectedTime = systemStartTime + m_totalPausedTime +
                                              static_cast<qint64>((frame.pts - firstPts) / m_playbackRate);
                        qint64 currentTime = av_gettime_relative();
                        waitTime = expectedTime - currentTime;

                        if (waitTime > adjustedDuration * 2)
                            waitTime = adjustedDuration * 2;
                        if (waitTime < -adjustedDuration * 3)
                        {
                            // 落后太多，重置时间基准
                            firstPts = frame.pts;
                            systemStartTime = av_gettime_relative() - m_totalPausedTime;
                            waitTime = 0;
                        }
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
                    // 使用微秒精度发送位置（毫秒）
                    emit positionChanged(frame.pts / 1000);
                    m_stats.totalFramesDisplayed++;
                    m_framesSinceLastFpsCalc++;
                    m_syncManager.frameDisplayed();
                }

                m_lastDisplayTime = av_gettime_relative();
            }
        }
        else if (!m_useBuffer)
        {
            // 非缓冲模式，在decodeVideoPacket中已处理
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

    // 自然播放到文件末尾（EOF）结束：通知上层，便于自动连播等后续处理
    if (eofReached)
    {
        emit playStateChanged(PlayerWidgetBase::StoppedState);
        emit mediaEnded(m_currentUrl);
    }

    qDebug() << "Decoder stopped - Decoded:" << m_stats.totalFramesDecoded
             << "Displayed:" << m_stats.totalFramesDisplayed
             << "Dropped:" << m_stats.droppedFrames;
}

// ==================== 视频解码与处理 ====================

void FFmpegDecoderThread::decodeVideoPacket(AVPacket *pkt)
{
    // 空指针检查
    if (!m_video.codecCtx || !pkt)
    {
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

        // 计算PTS（微秒）并归一化到播放时间轴。
        // 注意：必须在这里归一化一次，并与 m_seekTargetPts 用同一套时间轴比较；
        // 若这里用原始 pts 去比归一化目标，seek 到后半段（源时间戳已归零/回绕的部分）时
        // 每一帧都会被判定为"未到目标"而丢弃，表现为画面卡住不动。
        qint64 rawPts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
        rawPts = (rawPts == AV_NOPTS_VALUE) ? AV_NOPTS_VALUE
                                            : av_rescale_q(rawPts, m_video.timeBase, {1, 1000000});
        const qint64 pts = normalizeTimestamp(rawPts, m_videoClock);

        // 处理seek后的帧过滤
        if (m_seekDecodingActive && m_seekTargetPts > 0)
        {
            if (pts < m_seekTargetPts - 50000) // 小于目标50ms以上，丢弃
            {
                m_stats.droppedFrames++;
                av_frame_free(&frame);
                continue;
            }
            else
            {
                // 达到目标范围，停止过滤
                m_seekDecodingActive = false;
                m_seekTargetPts = -1;
                qDebug() << "Reached seek target at PTS:" << pts;
            }
        }

        if (m_useBuffer)
            processVideoFrameBuffered(frame, pts);
        else
            processVideoFrameDirect(frame, pts);

        av_frame_free(&frame);
    }
}

void FFmpegDecoderThread::processVideoFrameDirect(AVFrame *frame, qint64 pts)
{
    // pts 由调用方（decodeVideoPacket）归一化后传入，这里不再重复计算，
    // 否则时间轴会被推进两次

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

void FFmpegDecoderThread::processVideoFrameBuffered(AVFrame *frame, qint64 pts)
{
    if (!frame)
        return;

    // pts 已由调用方归一化，这里只做时长估计
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
    if (!m_audio.codecCtx || !pkt)
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

        // 计算时间戳并更新音频时钟（同样归一化，保证与视频共用一条时间轴）
        qint64 rawPts = (frame->pts == AV_NOPTS_VALUE) ? frame->pkt_dts : frame->pts;
        rawPts = (rawPts == AV_NOPTS_VALUE) ? AV_NOPTS_VALUE
                                            : av_rescale_q(rawPts, m_audio.timeBase, {1, 1000000});
        qint64 pts = normalizeTimestamp(rawPts, m_audioClock);
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
            int dataSize = realSamples * 4;
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
    return m_durationMs;
}

bool FFmpegDecoderThread::isPaused() const { return m_paused; }

void FFmpegDecoderThread::setPaused()
{
    QMutexLocker lock(&m_mutex);
    m_paused = !m_paused;
    
    qDebug() << "setPaused:" << m_paused;
    
    if (!m_paused)
    {
        // 恢复播放时唤醒等待线程
        m_pauseCondition.wakeAll();
    }
    
    emit playStateChanged(m_paused ? PlayerWidgetBase::PausedState : PlayerWidgetBase::PlayingState);
}

void FFmpegDecoderThread::seekTo(qint64 posMs)
{
    QMutexLocker lock(&m_mutex);
    m_seekPos = qBound(0LL, posMs, duration());
    m_seekRequested = true;
    
    // 如果处于暂停状态，唤醒线程处理seek
    if (m_paused)
    {
        m_pauseCondition.wakeAll();
    }
    
    qDebug() << "seekTo requested:" << m_seekPos << "ms";
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
            // close() 内部等待超时时会直接返回且不做清理，这里再确认一次，
            // 避免线程仍在运行时就被 delete（QThread 在运行中被销毁会直接终止进程）
            if (m_decoder_->isRunning())
            {
                qWarning() << "Decoder thread still running, waiting again...";
                m_decoder_->wait(3000);
            }
        }

        // 第三步：删除解码器对象
        delete m_decoder_;
        m_decoder_ = nullptr;

        qDebug() << "Decoder cleaned up";
    }
}

void FFmpegPlayer::shutdown()
{
    // 释放播放资源：停止解码线程、关闭媒体文件与音频输出。
    // 子控件不会收到 closeEvent，窗口关闭时必须由外层显式调用，
    // 否则解码线程、文件句柄与音频设备会一直占着不释放。
    if (m_isClosing && !m_decoder_)
        return;     // 已经释放过

    qDebug() << "FFmpegPlayer::shutdown";
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
}

void FFmpegPlayer::closeEvent(QCloseEvent *event)
{
    qDebug() << "FFmpegPlayer closeEvent";
    shutdown();
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
    // 视频区内的滚轮缩放/拖动平移：事件先落到子显示组件，统一由事件过滤器接管
    m_displayWidget_->installEventFilter(this);
    m_displayWidget_->show();

    updateViewTransform();
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
        updateLabelFrame();
}

void FFmpegPlayer::onVideoGeometryChanged(int width, int height, int sarNum, int sarDen)
{
    m_videoWidth_ = width;
    m_videoHeight_ = height;

    // 显示比例 = 像素宽比 * SAR（DVD 标清源必须考虑 SAR，否则会变形）
    if (width > 0 && height > 0 && sarNum > 0 && sarDen > 0)
        m_displayAspect_ = static_cast<double>(width) * sarNum / (static_cast<double>(height) * sarDen);
    else if (width > 0 && height > 0)
        m_displayAspect_ = static_cast<double>(width) / height;
    else
        m_displayAspect_ = 0.0;

    // 新的画面尺寸可能让原有缩放/平移越界，这里统一重算映射
    m_panOffset_ = QPointF();
    updateViewTransform();

    // 通知布局：推荐尺寸发生变化（便于窗口按视频比例自适应）
    updateGeometry();
    updateDisplay();
}

// ==================== 画面显示控制 ====================

double FFmpegPlayer::displayAspect() const
{
    if (m_aspectOverride_ > 0.0)
        return m_aspectOverride_;
    return m_displayAspect_;
}

void FFmpegPlayer::setScaleMode(VideoScaleMode mode)
{
    if (m_scaleMode_ == mode)
        return;

    m_scaleMode_ = mode;
    m_panOffset_ = QPointF();   // 模式切换后原有平移不再有意义
    updateViewTransform();
    updateDisplay();
}

void FFmpegPlayer::setAspectOverride(double aspect)
{
    const double normalized = (aspect > 0.0) ? aspect : 0.0;
    if (qFuzzyCompare(m_aspectOverride_ + 1.0, normalized + 1.0))
        return;

    m_aspectOverride_ = normalized;
    m_panOffset_ = QPointF();
    updateViewTransform();
    updateDisplay();
}

void FFmpegPlayer::setZoom(double zoom)
{
    const double clamped = qBound(kMinZoom, zoom, kMaxZoom);
    if (qFuzzyCompare(m_zoom_, clamped))
        return;

    m_zoom_ = clamped;
    updateViewTransform();
    updateDisplay();
    emit zoomChanged(m_zoom_);
}

void FFmpegPlayer::setPanOffset(const QPointF &offset)
{
    if (m_panOffset_ == offset)
        return;

    m_panOffset_ = offset;
    updateViewTransform();
    updateDisplay();
}

void FFmpegPlayer::resetView()
{
    const bool zoomWasChanged = !qFuzzyCompare(m_zoom_, 1.0);
    const bool panWasChanged = !m_panOffset_.isNull();

    m_zoom_ = 1.0;
    m_panOffset_ = QPointF();

    if (zoomWasChanged || panWasChanged)
    {
        updateViewTransform();
        updateDisplay();
    }
    if (zoomWasChanged)
        emit zoomChanged(m_zoom_);
}

QSize FFmpegPlayer::preferredVideoSize() const
{
    if (m_videoWidth_ <= 0 || m_videoHeight_ <= 0)
        return QSize();

    // 原始尺寸模式下推荐尺寸就是视频像素尺寸，其余模式按显示比例给出
    if (m_scaleMode_ == OriginalSize)
        return QSize(m_videoWidth_, m_videoHeight_);

    const double aspect = displayAspect();
    if (aspect <= 0.0)
        return QSize(m_videoWidth_, m_videoHeight_);

    return QSize(m_videoWidth_, qMax(1, qRound(m_videoWidth_ / aspect)));
}

void FFmpegPlayer::computeBaseScale(double &scaleX, double &scaleY) const
{
    scaleX = 1.0;
    scaleY = 1.0;

    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
        return;

    if (m_scaleMode_ == OriginalSize && m_videoWidth_ > 0 && m_videoHeight_ > 0)
    {
        // 原始尺寸：按 1:1 物理像素显示（HiDPI 下窗口逻辑尺寸需乘 dpr 才是物理像素）
        const double dpr = devicePixelRatioF();
        scaleX = m_videoWidth_ / (static_cast<double>(w) * dpr);
        scaleY = m_videoHeight_ / (static_cast<double>(h) * dpr);
        return;
    }

    const double widgetAspect = static_cast<double>(w) / h;
    double frameAspect = displayAspect();
    if (frameAspect <= 0.0)
        frameAspect = widgetAspect;

    switch (m_scaleMode_)
    {
    case StretchToWindow:
        break;      // 保持 1:1 直接铺满，画面会变形（用户明确选择的行为）
    case FillWindow:
        // 铺满窗口：等比放大到覆盖整个窗口，超出的方向由渲染层裁掉
        if (frameAspect > widgetAspect)
            scaleX = frameAspect / widgetAspect;
        else
            scaleY = widgetAspect / frameAspect;
        break;
    case FitWindow:
    default:
        // 适应窗口：等比缩放到完整可见，多余方向留黑边
        if (frameAspect > widgetAspect)
            scaleY = widgetAspect / frameAspect;
        else
            scaleX = frameAspect / widgetAspect;
        break;
    }
}

QPointF FFmpegPlayer::clampedPan(double baseScaleX, double baseScaleY) const
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
        return QPointF();

    // 只有画面超出窗口的方向才允许平移，且最多平移到边缘对齐
    const double maxPanX = qMax(0.0, (baseScaleX * m_zoom_ - 1.0) * 0.5 * w);
    const double maxPanY = qMax(0.0, (baseScaleY * m_zoom_ - 1.0) * 0.5 * h);
    return QPointF(qBound(-maxPanX, m_panOffset_.x(), maxPanX),
                   qBound(-maxPanY, m_panOffset_.y(), maxPanY));
}

void FFmpegPlayer::updateViewTransform()
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0)
        return;

    double baseScaleX = 1.0;
    double baseScaleY = 1.0;
    computeBaseScale(baseScaleX, baseScaleY);

    const QPointF pan = clampedPan(baseScaleX, baseScaleY);
    if (pan != m_panOffset_)
    {
        m_panOffset_ = pan;
        emit panOffsetChanged(m_panOffset_);
    }

    const double scaleX = baseScaleX * m_zoom_;
    const double scaleY = baseScaleY * m_zoom_;

    // 窗口像素 -> 归一化设备坐标；屏幕坐标 y 轴向下，NDC y 轴向上，故取负
    const float ndcX = static_cast<float>(m_panOffset_.x() * 2.0 / w);
    const float ndcY = static_cast<float>(-m_panOffset_.y() * 2.0 / h);

#if OPENGL_AVAILABLE
    if (m_glWidget_)
        m_glWidget_->setViewTransform(static_cast<float>(scaleX), static_cast<float>(scaleY), ndcX, ndcY);
#endif
}

void FFmpegPlayer::updateLabelFrame()
{
    if (m_currentFrame.isNull() || !m_displayLabel_)
        return;

    const QSize area = m_displayLabel_->size();
    if (area.isEmpty())
        return;

    const double w = area.width();
    const double h = area.height();
    const double frameW = m_currentFrame.width();
    const double frameH = m_currentFrame.height();
    if (frameW <= 0.0 || frameH <= 0.0)
        return;

    double baseScaleX = 1.0;
    double baseScaleY = 1.0;
    computeBaseScale(baseScaleX, baseScaleY);

    const double scaleX = baseScaleX * m_zoom_;
    const double scaleY = baseScaleY * m_zoom_;
    const double destW = w * scaleX;
    const double destH = h * scaleY;
    const QPointF pan = clampedPan(baseScaleX, baseScaleY);

    // 画面在窗口坐标系中的位置（未裁剪前）
    const double destX = (w - destW) * 0.5 + pan.x();
    const double destY = (h - destH) * 0.5 + pan.y();

    // 与窗口相交的可见区域（窗口坐标系）
    const double visLeft = qMax(0.0, destX);
    const double visTop = qMax(0.0, destY);
    const double visRight = qMin(w, destX + destW);
    const double visBottom = qMin(h, destY + destH);
    if (visRight - visLeft < 1.0 || visBottom - visTop < 1.0)
        return;

    // 换算到"画面自身坐标系"后裁剪源图，避免先放大整帧再裁掉的无谓开销
    const double sx = frameW / destW;
    const double sy = frameH / destH;
    QRect srcRect(qRound((visLeft - destX) * sx), qRound((visTop - destY) * sy),
                  qMax(1, qRound((visRight - visLeft) * sx)), qMax(1, qRound((visBottom - visTop) * sy)));
    srcRect = srcRect.intersected(QRect(0, 0, m_currentFrame.width(), m_currentFrame.height()));
    if (srcRect.isEmpty())
        return;

    // 可见区域在超出窗口的方向上必然铺满整个窗口，其余方向居中，QLabel 的居中对齐正好匹配
    const QSize target(qMax(1, qRound(visRight - visLeft)), qMax(1, qRound(visBottom - visTop)));
    m_displayLabel_->setPixmap(QPixmap::fromImage(m_currentFrame.copy(srcRect))
                                   .scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

void FFmpegPlayer::handleWheelZoom(QWheelEvent *event, const QPointF &posInWidget)
{
    if (!event)
        return;

    const int delta = event->angleDelta().y();
    if (delta == 0)
        return;

    const double oldZoom = m_zoom_;
    const double newZoom = qBound(kMinZoom, oldZoom * std::pow(1.0015, delta), kMaxZoom);
    if (qFuzzyCompare(oldZoom, newZoom))
        return;

    // 以光标位置为锚点缩放：保持光标下的画面内容不动
    const QPointF center(width() * 0.5, height() * 0.5);
    const QPointF anchor = posInWidget - center - m_panOffset_;
    const double ratio = newZoom / oldZoom;

    m_zoom_ = newZoom;
    m_panOffset_ = m_panOffset_ + anchor * (1.0 - ratio);

    updateViewTransform();
    updateDisplay();
    emit zoomChanged(m_zoom_);
}

bool FFmpegPlayer::handleDragEvent(QEvent *event, const QPointF &posInWidget)
{
    switch (event->type())
    {
    case QEvent::MouseButtonPress:
    {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton)
            return false;
        // 只有画面超出窗口（有可平移空间）时才启动拖动，否则保持默认行为
        double baseScaleX = 1.0;
        double baseScaleY = 1.0;
        computeBaseScale(baseScaleX, baseScaleY);
        if (width() * (baseScaleX * m_zoom_ - 1.0) <= 1.0 && height() * (baseScaleY * m_zoom_ - 1.0) <= 1.0)
            return false;

        m_dragging_ = true;
        m_dragStartPos_ = posInWidget;
        m_dragStartPan_ = m_panOffset_;
        return true;
    }
    case QEvent::MouseMove:
    {
        if (!m_dragging_)
            return false;
        m_panOffset_ = m_dragStartPan_ + (posInWidget - m_dragStartPos_);
        updateViewTransform();
        updateDisplay();
        return true;
    }
    case QEvent::MouseButtonRelease:
    {
        if (!m_dragging_)
            return false;
        m_dragging_ = false;
        return true;
    }
    default:
        break;
    }
    return false;
}

bool FFmpegPlayer::eventFilter(QObject *watched, QEvent *event)
{
    // 子显示组件铺满整个播放控件，两者坐标系一致，可直接使用子组件的坐标
    if (watched == m_displayWidget_)
    {
        switch (event->type())
        {
        case QEvent::Wheel:
            handleWheelZoom(static_cast<QWheelEvent *>(event),
                            static_cast<QWheelEvent *>(event)->position());
            return true;
        case QEvent::MouseButtonPress:
            // 不拦截按下事件：必须让子组件真正收到它，Qt 才会建立隐式鼠标抓取，
            // 后续的 move/release 才能稳定送到拖动逻辑（过滤器里吃掉按下会导致拖动失效）
            handleDragEvent(event, static_cast<QMouseEvent *>(event)->position());
            return false;
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
            return handleDragEvent(event, static_cast<QMouseEvent *>(event)->position());
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FFmpegPlayer::wheelEvent(QWheelEvent *event)
{
    handleWheelZoom(event, event->position());
    event->accept();
}

void FFmpegPlayer::mousePressEvent(QMouseEvent *event)
{
    if (!handleDragEvent(event, event->position()))
        QWidget::mousePressEvent(event);
}

void FFmpegPlayer::mouseMoveEvent(QMouseEvent *event)
{
    if (!handleDragEvent(event, event->position()))
        QWidget::mouseMoveEvent(event);
}

void FFmpegPlayer::mouseReleaseEvent(QMouseEvent *event)
{
    if (!handleDragEvent(event, event->position()))
        QWidget::mouseReleaseEvent(event);
}

QSize FFmpegPlayer::sizeHint() const
{
    const QSize preferred = preferredVideoSize();
    if (!preferred.isEmpty())
        return preferred;
    return QWidget::sizeHint();
}

QSize FFmpegPlayer::minimumSizeHint() const
{
    return QSize(160, 90); // 允许自由缩小窗口，不再被视频原始尺寸卡住
}

void FFmpegPlayer::resizeEvent(QResizeEvent *event)
{
    if (m_displayWidget_)
    {
        m_displayWidget_->setGeometry(rect());
    }

    // 窗口尺寸变化后重新计算画面映射（缩放模式/倍率/平移都依赖窗口尺寸）
    updateViewTransform();

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
    // ========== 自然播放结束（EOF）转发 ==========
    connect(m_decoder_, &FFmpegDecoderThread::mediaEnded, m_playerCore_, &PlayerWidgetBase::mediaFinished, Qt::QueuedConnection);
    // ========== 视频画面几何（含像素宽高比）转发 ==========
    connect(m_decoder_, &FFmpegDecoderThread::videoGeometryChanged,
            m_playerCore_, &PlayerWidgetBase::videoGeometryChanged, Qt::QueuedConnection);
    connect(m_decoder_, &FFmpegDecoderThread::videoGeometryChanged,
            this, &FFmpegPlayer::onVideoGeometryChanged, Qt::QueuedConnection);

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