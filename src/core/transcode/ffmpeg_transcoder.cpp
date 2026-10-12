#include "ffmpeg_transcoder.h"

#ifdef CAN_USE_FFMPEG

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <cmath>

namespace
{
// FFmpeg错误码转字符串（av_err2str 在 MSVC 下不可用，统一用 av_strerror）
QString avErrorString(int errCode)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errCode, errbuf, sizeof(errbuf));
    return QString::fromUtf8(errbuf);
}

// 计算显示宽高比（结合像素宽高比 SAR）
double displayAspect(int width, int height, AVRational sar)
{
    if (width <= 0 || height <= 0)
        return 0.0;
    if (sar.num > 0 && sar.den > 0)
        return static_cast<double>(width) * sar.num / (static_cast<double>(height) * sar.den);
    return static_cast<double>(width) / height;
}

// H.264 常见外挂编码器（按优先级探测，FFmpeg 自身不带 H.264 编码器）
const char *const kH264EncoderNames[] = {
    "libx264", "h264_mf", "h264_qsv", "h264_nvenc", "h264_amf", "h264_vaapi", "h264_v4l2m2m"
};

// 流拷贝时单个包允许的最大时间跨度（微秒）。
// 正常视频帧间隔约 40ms、音频包约 32ms，1 秒足以覆盖正常情况；
// 超出该范围说明输入时间戳异常（VOB 内的 PTS 跳变/回绕等），按异常处理。
const int64_t kCopyMaxStepUs = 1000000;
} // namespace

FFmpegTranscoder::FFmpegTranscoder(QObject *parent)
    : QThread(parent)
{
}

FFmpegTranscoder::~FFmpegTranscoder()
{
    cancel();
    if (isRunning())
        wait(5000);
    cleanupAll();
}

void FFmpegTranscoder::setOption(const TranscodeOption &option)
{
    m_option = option;
}

void FFmpegTranscoder::cancel()
{
    m_cancel.store(true);
}

// ==================== 编码器探测 ====================

const AVCodec *FFmpegTranscoder::findEncoder(AVCodecID codecId)
{
    const AVCodec *codec = avcodec_find_encoder(codecId);
    if (codec)
        return codec;

    // 外挂编码器探测（例如 Windows 上的 h264_mf / 带 x264 时的 libx264）
    if (codecId == AV_CODEC_ID_H264)
    {
        for (const char *name : kH264EncoderNames)
        {
            codec = avcodec_find_encoder_by_name(name);
            if (codec)
                return codec;
        }
    }
    return nullptr;
}

bool FFmpegTranscoder::isEncoderAvailable(AVCodecID codecId)
{
    return findEncoder(codecId) != nullptr;
}

QStringList FFmpegTranscoder::availableEncoderNames(AVCodecID codecId)
{
    QStringList names;
    void *iter = nullptr;
    const AVCodec *codec = nullptr;
    while ((codec = av_codec_iterate(&iter)))
    {
        if (!av_codec_is_encoder(codec) || codec->id != codecId)
            continue;
        names << QString::fromUtf8(codec->name);
    }
    names.removeDuplicates();
    names.sort();
    return names;
}

bool FFmpegTranscoder::isOutputFormatAvailable(const QString &filePath)
{
    // 通过输出文件名探测封装格式是否可用
    const AVOutputFormat *fmt = av_guess_format(nullptr, filePath.toUtf8().constData(), nullptr);
    return fmt != nullptr;
}

QSize FFmpegTranscoder::probeVideoResolution(const QString &filePath)
{
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, filePath.toUtf8().constData(), nullptr, nullptr) != 0)
        return QSize();

    avformat_find_stream_info(ctx, nullptr);
    QSize size;
    const int idx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx >= 0)
        size = QSize(ctx->streams[idx]->codecpar->width, ctx->streams[idx]->codecpar->height);

    avformat_close_input(&ctx);
    return size;
}

AVPixelFormat FFmpegTranscoder::pickPixelFormat(const AVCodec *codec, AVPixelFormat src)
{
    if (!codec || !codec->pix_fmts)
        return src;
    for (int i = 0; codec->pix_fmts[i] != AV_PIX_FMT_NONE; ++i)
    {
        if (codec->pix_fmts[i] == src)
            return src;
    }
    return codec->pix_fmts[0];
}

enum AVSampleFormat FFmpegTranscoder::pickSampleFormat(const AVCodec *codec, enum AVSampleFormat src)
{
    if (!codec || !codec->sample_fmts)
        return src;
    for (int i = 0; codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE; ++i)
    {
        if (codec->sample_fmts[i] == src)
            return src;
    }
    return codec->sample_fmts[0];
}

int FFmpegTranscoder::pickSampleRate(const AVCodec *codec, int srcRate)
{
    if (!codec || !codec->supported_samplerates || srcRate <= 0)
        return srcRate;
    for (int i = 0; codec->supported_samplerates[i] != 0; ++i)
    {
        if (codec->supported_samplerates[i] == srcRate)
            return srcRate;
    }
    return codec->supported_samplerates[0];
}

// ==================== 转码主流程 ====================

void FFmpegTranscoder::run()
{
    QString errorInfo;
    bool success = false;

    emit stateInfo(QObject::tr("preparing transcode..."));

    do
    {
        if (m_option.inputFiles.isEmpty() || m_option.outputFile.isEmpty())
        {
            errorInfo = QObject::tr("invalid input/output path");
            break;
        }

        if (!isOutputFormatAvailable(m_option.outputFile))
        {
            errorInfo = QObject::tr("unsupported output format: %1").arg(m_option.outputFile);
            break;
        }

        // 统计输入总字节数（用于进度计算）
        // 不用容器的 duration 元数据：部分 VOB 的 duration 本身就是错的
        // （实测 D:\Temp 那碟：容器写 998s，按包时间戳与音频帧数推算实际 2437.8s），
        // 用它算进度会导致进度条乱跳或长期卡住。字节数则是可靠的。
        m_totalInputBytes = 0;
        for (const QString &file : m_option.inputFiles)
            m_totalInputBytes += qMax<qint64>(0, QFileInfo(file).size());
        m_doneInputBytes = 0;
        m_skippedPackets = 0;

        const int fileCount = m_option.inputFiles.size();
        for (int i = 0; i < fileCount; ++i)
        {
            if (m_cancel.load())
                break;

            const QString &path = m_option.inputFiles.at(i);
            emit stateInfo(QObject::tr("processing %1/%2: %3")
                               .arg(i + 1).arg(fileCount).arg(QDir::toNativeSeparators(path)));

            if (!processInput(path, errorInfo))
                break;

            m_doneInputBytes += qMax<qint64>(0, QFileInfo(path).size());
        }

        if (!errorInfo.isEmpty())
            break;

        if (m_cancel.load())
        {
            errorInfo = QObject::tr("transcode canceled");
            break;
        }

        // 收尾：刷新编码器并写入文件尾
        if (m_video.enabled && !m_video.copy)
            flushEncoder(m_video, errorInfo);
        if (m_audio.enabled && !m_audio.copy)
            flushEncoder(m_audio, errorInfo);

        if (m_outCtx && m_headerWritten)
            av_write_trailer(m_outCtx);

        success = errorInfo.isEmpty();
        if (success)
        {
            reportProgress(100);
            if (m_skippedPackets > 0)
            {
                // 源文件有损坏数据：已尽量跳过并保留其余内容，这里明确告知用户
                emit stateInfo(QObject::tr("transcode finished, %1 damaged frame(s) skipped")
                                   .arg(static_cast<qlonglong>(m_skippedPackets)));
            }
            else
            {
                emit stateInfo(QObject::tr("transcode finished"));
            }
        }
    } while (false);

    if (!errorInfo.isEmpty())
    {
        if (m_cancel.load() && !success)
            errorInfo = QObject::tr("transcode canceled");
        emit errorOccurred(errorInfo);
    }

    // 取消时删除未完成的输出文件，避免产生半截文件
    if (!success && m_outCtx && m_outCtx->url[0] != '\0')
    {
        const QString outPath = QString::fromUtf8(m_outCtx->url);
        cleanupAll();
        QFile::remove(outPath);
    }
    else
    {
        cleanupAll();
    }

    emit transcodeFinished(success, success ? QObject::tr("output: %1").arg(m_option.outputFile) : errorInfo);
}

bool FFmpegTranscoder::processInput(const QString &path, QString &errorInfo)
{
    // 打开输入
    m_inCtx = nullptr;
    int ret = avformat_open_input(&m_inCtx, path.toUtf8().constData(), nullptr, nullptr);
    if (ret < 0)
    {
        errorInfo = QObject::tr("failed to open input: %1").arg(avErrorString(ret));
        return false;
    }
    if (avformat_find_stream_info(m_inCtx, nullptr) < 0)
    {
        errorInfo = QObject::tr("failed to read stream info");
        return false;
    }

    const bool firstInput = (m_video.inIndex < 0 && m_audio.inIndex < 0) || !m_outCtx;
    if (firstInput)
    {
        // 首个输入：确定输出流（解码器/编码器）并写入文件头
        if (!prepareOutput(errorInfo))
            return false;

        if (!(m_outCtx->oformat->flags & AVFMT_NOFILE))
        {
            ret = avio_open(&m_outCtx->pb, m_option.outputFile.toUtf8().constData(), AVIO_FLAG_WRITE);
            if (ret < 0)
            {
                errorInfo = QObject::tr("failed to open output: %1").arg(avErrorString(ret));
                return false;
            }
        }

        ret = avformat_write_header(m_outCtx, nullptr);
        if (ret < 0)
        {
            errorInfo = QObject::tr("failed to write header (container may not support the codec): %1")
                            .arg(avErrorString(ret));
            return false;
        }
        m_headerWritten = true;
    }
    else
    {
        // 后续分段：重建解码器（编码器与输出流保持不变，实现多段合并）
        releaseStream(m_video);
        releaseStream(m_audio);
        setupDecoder(m_video, AVMEDIA_TYPE_VIDEO);
        setupDecoder(m_audio, AVMEDIA_TYPE_AUDIO);

        // 输出流只在首段创建，后续分段必须与之编码参数一致，否则写出的文件是坏的。
        // DVD 的不同 VTS 段落（甚至同一标题集的不同段落）音频编码可能不同，这里提前给出明确报错。
        if (!checkSegmentCompatible(m_video, AVMEDIA_TYPE_VIDEO, errorInfo) ||
            !checkSegmentCompatible(m_audio, AVMEDIA_TYPE_AUDIO, errorInfo))
            return false;
    }

    // 读取并处理数据包
    AVPacket *pkt = av_packet_alloc();
    QElapsedTimer reportTimer;
    reportTimer.start();

    while (!m_cancel.load() && av_read_frame(m_inCtx, pkt) >= 0)
    {
        bool ok = true;
        if (m_video.enabled && pkt->stream_index == m_video.inIndex)
            ok = handleVideoPacket(m_video, pkt, errorInfo);
        else if (m_audio.enabled && pkt->stream_index == m_audio.inIndex)
            ok = handleAudioPacket(m_audio, pkt, errorInfo);

        av_packet_unref(pkt);
        if (!ok)
        {
            av_packet_free(&pkt);
            return false;
        }

        // 进度上报（按整数百分比变化节流）：
        // 用"已读取的输入字节 / 输入总字节"，不依赖容器 duration 元数据
        if (reportTimer.elapsed() > 500 && m_totalInputBytes > 0 && m_inCtx->pb)
        {
            reportTimer.restart();
            const int64_t pos = avio_tell(m_inCtx->pb);
            const double ratio = static_cast<double>(m_doneInputBytes + qMax<int64_t>(0, pos)) / m_totalInputBytes;
            reportProgress(qBound(0, static_cast<int>(ratio * 100), 100));
        }
    }
    av_packet_free(&pkt);

    if (m_cancel.load())
        return true;

    // 段末：刷出解码器中残留的帧（编码器跨段复用，不在此刷新）
    if (m_video.enabled && !m_video.copy)
        flushDecoder(m_video, errorInfo);
    if (m_audio.enabled && !m_audio.copy)
        flushDecoder(m_audio, errorInfo);

    // 流拷贝的时间轴状态（copyLastDts/copyLastInDts/copyLastStep）跨分段复用，
    // 段与段之间无需额外换算偏移：下一段首包会按"上一个正常步长"自然接续。

    // 关闭输入（保存下一段使用的解码器上下文已在下次重建）
    if (m_inCtx)
    {
        avformat_close_input(&m_inCtx);
        m_inCtx = nullptr;
    }
    return errorInfo.isEmpty();
}

bool FFmpegTranscoder::prepareOutput(QString &errorInfo)
{
    // 定位输入流
    if (!setupDecoder(m_video, AVMEDIA_TYPE_VIDEO))
        m_video.enabled = false;     // 无视频流（纯音频转换）
    if (!setupDecoder(m_audio, AVMEDIA_TYPE_AUDIO))
        m_audio.enabled = false;     // 无音频流（无声视频）

    if (!m_video.enabled && !m_audio.enabled)
    {
        errorInfo = QObject::tr("no valid video/audio stream found");
        return false;
    }

    // 创建输出上下文（按扩展名猜测封装格式）
    int ret = avformat_alloc_output_context2(&m_outCtx, nullptr, nullptr,
                                             m_option.outputFile.toUtf8().constData());
    if (ret < 0 || !m_outCtx)
    {
        errorInfo = QObject::tr("failed to create output context: %1").arg(m_option.outputFile);
        return false;
    }

    // 视频 / 音频输出流
    if (m_video.enabled && !setupEncoder(m_video, AVMEDIA_TYPE_VIDEO, errorInfo))
        return false;
    if (m_audio.enabled && !setupEncoder(m_audio, AVMEDIA_TYPE_AUDIO, errorInfo))
        return false;

    return true;
}

bool FFmpegTranscoder::setupDecoder(StreamCtx &sc, AVMediaType type)
{
    sc.inIndex = av_find_best_stream(m_inCtx, type, -1, -1, nullptr, 0);
    if (sc.inIndex < 0)
        return false;

    sc.inStream = m_inCtx->streams[sc.inIndex];
    sc.copy = (type == AVMEDIA_TYPE_VIDEO) ? m_option.videoCopy : m_option.audioCopy;
    if (sc.copy)
    {
        sc.enabled = true;   // 流拷贝模式不需要解码器，但流本身有效
        return true;
    }

    const AVCodec *dec = avcodec_find_decoder(sc.inStream->codecpar->codec_id);
    if (!dec)
        return false;

    sc.decCtx = avcodec_alloc_context3(dec);
    if (!sc.decCtx || avcodec_parameters_to_context(sc.decCtx, sc.inStream->codecpar) < 0)
        return false;

    // 容错：源文件有损坏数据时尽量解出可解的部分，而不是遇到一个坏包就中断整个转码
    sc.decCtx->err_recognition = AV_EF_IGNORE_ERR;

    if (avcodec_open2(sc.decCtx, dec, nullptr) < 0)
    {
        avcodec_free_context(&sc.decCtx);
        return false;
    }

    sc.decFrame = av_frame_alloc();
    sc.enabled = true;
    return true;
}

bool FFmpegTranscoder::setupEncoder(StreamCtx &sc, AVMediaType type, QString &errorInfo)
{
    const bool isVideo = (type == AVMEDIA_TYPE_VIDEO);

    if (sc.copy)
    {
        // 流拷贝：直接复制编码参数（无损、速度快）
        // 先检查输出容器是否支持该编码（如 VOB 的 MPEG-2 视频无法放入 MP4）
        if (avformat_query_codec(m_outCtx->oformat, sc.inStream->codecpar->codec_id,
                                 FF_COMPLIANCE_NORMAL) == 0)
        {
            errorInfo = QObject::tr("container %1 does not support %2 codec; "
                                    "choose another container or re-encode instead of copy")
                            .arg(QFileInfo(m_option.outputFile).suffix().toUpper(),
                                 avcodec_get_name(sc.inStream->codecpar->codec_id));
            return false;
        }
        sc.outStream = avformat_new_stream(m_outCtx, nullptr);
        if (!sc.outStream)
        {
            errorInfo = QObject::tr("failed to create output stream");
            return false;
        }
        if (avcodec_parameters_copy(sc.outStream->codecpar, sc.inStream->codecpar) < 0)
        {
            errorInfo = QObject::tr("failed to copy codec parameters");
            return false;
        }
        sc.outStream->codecpar->codec_tag = 0;
        // 时间基沿用输入流，保证拷贝时时间戳换算正确
        sc.outStream->time_base = sc.inStream->time_base;
        sc.outIndex = sc.outStream->index;
        sc.enabled = true;
        return true;
    }

    const AVCodecID codecId = isVideo ? m_option.videoCodec : m_option.audioCodec;
    const AVCodec *enc = findEncoder(codecId);
    if (!enc)
    {
        errorInfo = QObject::tr("encoder not available for %1 (%2)")
                        .arg(isVideo ? QObject::tr("video") : QObject::tr("audio"))
                        .arg(avcodec_get_name(codecId));
        return false;
    }

    sc.encCtx = avcodec_alloc_context3(enc);
    if (!sc.encCtx)
    {
        errorInfo = QObject::tr("failed to allocate encoder context");
        return false;
    }

    if (isVideo)
    {
        const int srcW = sc.decCtx->width;
        const int srcH = sc.decCtx->height;
        AVRational sar = sc.decCtx->sample_aspect_ratio;
        if (sar.num <= 0 || sar.den <= 0)
            sar = av_make_q(1, 1);

        int dstW = (m_option.outWidth > 0) ? m_option.outWidth : srcW;
        int dstH = (m_option.outHeight > 0) ? m_option.outHeight : srcH;

        // 保持显示比例：在目标尺寸框内按真实显示比例取实际尺寸
        const double dar = displayAspect(srcW, srcH, sar);
        if (m_option.keepDisplayAspect)
        {
            if (dar > 0.0 && dstW > 0 && dstH > 0)
            {
                const double boxAspect = static_cast<double>(dstW) / dstH;
                if (boxAspect > dar)
                    dstW = qRound(dstH * dar);
                else
                    dstH = qRound(dstW / dar);
            }
            // 重算 SAR，保证缩放后显示比例不变（DVD 的 720x480/720x576 尤其重要）
            if (dar > 0.0 && dstH > 0)
                sar = av_d2q((dar * dstH) / dstW, 1024);
        }
        // 未缩放时保持从解码器取到的原始 SAR（dar 未改动）

        dstW = qMax(2, dstW - (dstW % 2));   // 保证偶数尺寸（多数编码器要求）
        dstH = qMax(2, dstH - (dstH % 2));

        AVRational frameRate = sc.inStream->avg_frame_rate;
        if (frameRate.num <= 0 || frameRate.den <= 0)
            frameRate = sc.inStream->r_frame_rate;
        if (frameRate.num <= 0 || frameRate.den <= 0)
            frameRate = av_make_q(25, 1);

        sc.encCtx->width = dstW;
        sc.encCtx->height = dstH;
        sc.encCtx->pix_fmt = pickPixelFormat(enc, sc.decCtx->pix_fmt);
        sc.encCtx->bit_rate = m_option.videoBitrate;
        sc.encCtx->framerate = frameRate;
        sc.encCtx->time_base = av_inv_q(frameRate);
        sc.encCtx->sample_aspect_ratio = sar;
        sc.encCtx->gop_size = 12;
        sc.encCtx->max_b_frames = 2;
        sc.outWidth = dstW;
        sc.outHeight = dstH;
    }
    else
    {
        sc.encCtx->sample_rate = pickSampleRate(enc, sc.decCtx->sample_rate);
        sc.encCtx->ch_layout = sc.decCtx->ch_layout;
        sc.encCtx->sample_fmt = pickSampleFormat(enc, sc.decCtx->sample_fmt);
        sc.encCtx->bit_rate = m_option.audioBitrate;
        sc.encCtx->time_base = av_make_q(1, sc.encCtx->sample_rate);
    }

    if (m_outCtx->oformat->flags & AVFMT_GLOBALHEADER)
        sc.encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(sc.encCtx, enc, nullptr) < 0)
    {
        errorInfo = QObject::tr("failed to open encoder: %1").arg(avcodec_get_name(codecId));
        return false;
    }

    sc.outStream = avformat_new_stream(m_outCtx, nullptr);
    if (!sc.outStream)
    {
        errorInfo = QObject::tr("failed to create output stream");
        return false;
    }
    if (avcodec_parameters_from_context(sc.outStream->codecpar, sc.encCtx) < 0)
    {
        errorInfo = QObject::tr("failed to copy encoder parameters");
        return false;
    }
    // 输出流时间基交给封装器决定，写入前统一换算
    sc.outStream->time_base = sc.encCtx->time_base;
    sc.outIndex = sc.outStream->index;

    // 准备转换后的待编码帧
    sc.convFrame = av_frame_alloc();
    if (isVideo)
    {
        sc.convFrame->format = sc.encCtx->pix_fmt;
        sc.convFrame->width = sc.encCtx->width;
        sc.convFrame->height = sc.encCtx->height;
    }
    else
    {
        sc.convFrame->format = sc.encCtx->sample_fmt;
        sc.convFrame->ch_layout = sc.encCtx->ch_layout;
        sc.convFrame->sample_rate = sc.encCtx->sample_rate;
        sc.convFrame->nb_samples = sc.encCtx->frame_size > 0 ? sc.encCtx->frame_size : 1024;
    }
    if (av_frame_get_buffer(sc.convFrame, 0) < 0)
    {
        errorInfo = QObject::tr("failed to allocate frame buffer");
        return false;
    }

    // 音频重采样上下文（与项目内播放器一致，使用 av_opt_set_* 配置）
    if (!isVideo)
    {
        sc.swrCtx = swr_alloc();
        av_opt_set_chlayout(sc.swrCtx, "in_chlayout", &sc.decCtx->ch_layout, 0);
        av_opt_set_int(sc.swrCtx, "in_sample_rate", sc.decCtx->sample_rate, 0);
        av_opt_set_sample_fmt(sc.swrCtx, "in_sample_fmt", sc.decCtx->sample_fmt, 0);
        av_opt_set_chlayout(sc.swrCtx, "out_chlayout", &sc.encCtx->ch_layout, 0);
        av_opt_set_int(sc.swrCtx, "out_sample_rate", sc.encCtx->sample_rate, 0);
        av_opt_set_sample_fmt(sc.swrCtx, "out_sample_fmt", sc.encCtx->sample_fmt, 0);
        if (swr_init(sc.swrCtx) < 0)
        {
            errorInfo = QObject::tr("failed to init resampler");
            return false;
        }
    }

    sc.enabled = true;
    return true;
}

bool FFmpegTranscoder::checkSegmentCompatible(StreamCtx &sc, AVMediaType type, QString &errorInfo)
{
    if (!sc.enabled || !sc.outStream)
        return true;    // 该流不参与输出，无需校验
    if (!sc.copy)
        return true;    // 重新编码模式：各段都按同一编码器输出，天然兼容

    const QString typeName = (type == AVMEDIA_TYPE_VIDEO) ? QObject::tr("video") : QObject::tr("audio");

    if (sc.inIndex < 0 || !sc.inStream)
    {
        errorInfo = QObject::tr("this segment has no %1 stream while the first segment does; "
                                "remove it from the input list or transcode it separately")
                        .arg(typeName);
        return false;
    }

    const AVCodecID inId = sc.inStream->codecpar->codec_id;
    const AVCodecID outId = sc.outStream->codecpar->codec_id;
    if (inId != outId)
    {
        errorInfo = QObject::tr("segment %1 codec (%2) differs from the first segment (%3); "
                                "they cannot be merged into one file, please transcode separately")
                        .arg(typeName,
                             QString::fromUtf8(avcodec_get_name(inId)),
                             QString::fromUtf8(avcodec_get_name(outId)));
        return false;
    }
    return true;
}

bool FFmpegTranscoder::handleVideoPacket(StreamCtx &sc, AVPacket *pkt, QString &errorInfo)
{
    if (sc.copy)
    {
        // 流拷贝：换算到输出时基后交给统一的时间轴处理（多段拼接、时间戳跳变都在其中吸收）
        AVPacket *outPkt = av_packet_alloc();
        if (!outPkt)
        {
            errorInfo = QObject::tr("failed to allocate packet");
            return false;
        }
        av_packet_ref(outPkt, pkt);
        av_packet_rescale_ts(outPkt, sc.inStream->time_base, sc.outStream->time_base);
        applyCopyPtsOffset(sc, outPkt);
        outPkt->stream_index = sc.outIndex;
        const int ret = av_interleaved_write_frame(m_outCtx, outPkt);
        av_packet_free(&outPkt);
        if (ret < 0)
        {
            errorInfo = QObject::tr("failed to write video packet: %1").arg(avErrorString(ret));
            return false;
        }
        return true;
    }

    int ret = avcodec_send_packet(sc.decCtx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
    {
        // 源数据损坏：跳过这一帧继续，尽量保住其余内容（而不是整个转码失败）
        if (ret == AVERROR_INVALIDDATA)
        {
            ++m_skippedPackets;
            return true;
        }
        errorInfo = QObject::tr("failed to send video packet: %1").arg(avErrorString(ret));
        return false;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_frame(sc.decCtx, sc.decFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret == AVERROR_INVALIDDATA)
        {
            // 解出坏帧：跳过这一帧，继续处理后续数据
            ++m_skippedPackets;
            break;
        }
        if (ret < 0)
        {
            errorInfo = QObject::tr("failed to receive video frame: %1").arg(avErrorString(ret));
            return false;
        }

        if (!convertVideoFrame(sc, sc.decFrame))
        {
            errorInfo = QObject::tr("failed to convert video frame");
            return false;
        }

        ret = avcodec_send_frame(sc.encCtx, sc.convFrame);
        if (ret < 0)
        {
            errorInfo = QObject::tr("failed to send frame to encoder: %1").arg(avErrorString(ret));
            return false;
        }
        if (!writeEncodedPackets(sc, errorInfo))
            return false;

        av_frame_unref(sc.decFrame);
    }
    return true;
}

bool FFmpegTranscoder::handleAudioPacket(StreamCtx &sc, AVPacket *pkt, QString &errorInfo)
{
    if (sc.copy)
    {
        AVPacket *outPkt = av_packet_alloc();
        if (!outPkt)
        {
            errorInfo = QObject::tr("failed to allocate packet");
            return false;
        }
        av_packet_ref(outPkt, pkt);
        av_packet_rescale_ts(outPkt, sc.inStream->time_base, sc.outStream->time_base);
        applyCopyPtsOffset(sc, outPkt);
        outPkt->stream_index = sc.outIndex;
        const int ret = av_interleaved_write_frame(m_outCtx, outPkt);
        av_packet_free(&outPkt);
        if (ret < 0)
        {
            errorInfo = QObject::tr("failed to write audio packet: %1").arg(avErrorString(ret));
            return false;
        }
        return true;
    }

    int ret = avcodec_send_packet(sc.decCtx, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
    {
        // 源数据损坏（如 VOB 尾部截断的 AC3 帧）：跳过继续，不中断整个转码
        if (ret == AVERROR_INVALIDDATA)
        {
            ++m_skippedPackets;
            return true;
        }
        errorInfo = QObject::tr("failed to send audio packet: %1").arg(avErrorString(ret));
        return false;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_frame(sc.decCtx, sc.decFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret == AVERROR_INVALIDDATA)
        {
            ++m_skippedPackets;
            break;
        }
        if (ret < 0)
        {
            errorInfo = QObject::tr("failed to receive audio frame: %1").arg(avErrorString(ret));
            return false;
        }

        // 重采样后可能得到 0~N 个输出帧（受编码器帧长约束）
        AVFrame *out = nullptr;
        if (!convertAudioFrame(sc, sc.decFrame, &out))
        {
            errorInfo = QObject::tr("failed to resample audio frame");
            return false;
        }
        while (out)
        {
            ret = avcodec_send_frame(sc.encCtx, out);
            if (ret < 0)
            {
                errorInfo = QObject::tr("failed to send audio frame to encoder: %1").arg(avErrorString(ret));
                return false;
            }
            if (!writeEncodedPackets(sc, errorInfo))
                return false;
            if (!convertAudioFrame(sc, nullptr, &out))
                break;
        }

        av_frame_unref(sc.decFrame);
    }
    return true;
}

bool FFmpegTranscoder::writeEncodedPackets(StreamCtx &sc, QString &errorInfo)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        errorInfo = QObject::tr("failed to allocate packet");
        return false;
    }

    while (true)
    {
        int ret = avcodec_receive_packet(sc.encCtx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
        {
            errorInfo = QObject::tr("failed to receive encoded packet: %1").arg(avErrorString(ret));
            av_packet_free(&pkt);
            return false;
        }

        pkt->stream_index = sc.outIndex;
        av_packet_rescale_ts(pkt, sc.encCtx->time_base, sc.outStream->time_base);
        ret = av_interleaved_write_frame(m_outCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
        {
            errorInfo = QObject::tr("failed to write packet: %1").arg(avErrorString(ret));
            av_packet_free(&pkt);
            return false;
        }
    }

    av_packet_free(&pkt);
    return true;
}

bool FFmpegTranscoder::flushDecoder(StreamCtx &sc, QString &errorInfo)
{
    if (!sc.decCtx)
        return true;

    AVPacket *pkt = av_packet_alloc();
    int ret = avcodec_send_packet(sc.decCtx, pkt);   // nullptr 数据表示刷新
    av_packet_free(&pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN))
        return true;

    while (true)
    {
        ret = avcodec_receive_frame(sc.decCtx, sc.decFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            break;

        if (sc.encCtx && sc.encCtx->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            if (!convertVideoFrame(sc, sc.decFrame))
                break;
            ret = avcodec_send_frame(sc.encCtx, sc.convFrame);
        }
        else
        {
            AVFrame *out = nullptr;
            if (!convertAudioFrame(sc, sc.decFrame, &out))
                break;
            while (out)
            {
                ret = avcodec_send_frame(sc.encCtx, out);
                if (!convertAudioFrame(sc, nullptr, &out))
                    break;
            }
        }
        if (ret < 0)
            break;
        if (!writeEncodedPackets(sc, errorInfo))
            return false;
        av_frame_unref(sc.decFrame);
    }
    return true;
}

bool FFmpegTranscoder::flushEncoder(StreamCtx &sc, QString &errorInfo)
{
    if (!sc.encCtx)
        return true;

    // 音频：先取出重采样器内的残留数据
    if (sc.swrCtx)
    {
        AVFrame *out = nullptr;
        while (convertAudioFrame(sc, nullptr, &out) && out)
        {
            const int ret = avcodec_send_frame(sc.encCtx, out);
            if (ret < 0)
                break;
            if (!writeEncodedPackets(sc, errorInfo))
                return false;
        }
    }

    avcodec_send_frame(sc.encCtx, nullptr);   // 刷新编码器
    return writeEncodedPackets(sc, errorInfo);
}

bool FFmpegTranscoder::convertVideoFrame(StreamCtx &sc, AVFrame *src)
{
    if (!src || !sc.encCtx)
        return false;

    sc.swsCtx = sws_getCachedContext(sc.swsCtx,
                                     src->width, src->height,
                                     static_cast<AVPixelFormat>(src->format),
                                     sc.encCtx->width, sc.encCtx->height,
                                     sc.encCtx->pix_fmt,
                                     SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sc.swsCtx)
        return false;

    if (av_frame_make_writable(sc.convFrame) < 0)
        return false;

    const int ret = sws_scale(sc.swsCtx, src->data, src->linesize, 0, src->height,
                              sc.convFrame->data, sc.convFrame->linesize);
    if (ret < 0)
        return false;

    // 按帧计数生成连续时间轴（多段合并也不会出现时间戳回跳）
    sc.convFrame->pts = sc.frameIndex++;
    return true;
}

bool FFmpegTranscoder::convertAudioFrame(StreamCtx &sc, AVFrame *src, AVFrame **out)
{
    *out = nullptr;
    if (!sc.swrCtx || !sc.encCtx || !sc.convFrame)
        return false;

    const int wanted = sc.encCtx->frame_size > 0 ? sc.encCtx->frame_size : (src ? src->nb_samples : 1024);
    if (av_frame_make_writable(sc.convFrame) < 0)
        return false;
    if (wanted > 0 && sc.convFrame->nb_samples != wanted)
        sc.convFrame->nb_samples = wanted;

    const uint8_t **inData = src ? const_cast<const uint8_t **>(src->data) : nullptr;
    const int inCount = src ? src->nb_samples : 0;
    const int got = swr_convert(sc.swrCtx, sc.convFrame->data, wanted, inData, inCount);
    if (got < 0)
        return false;
    if (got == 0)
        return true;    // 采样数不足一帧，继续缓冲

    sc.convFrame->nb_samples = got;
    sc.convFrame->pts = av_rescale_q(sc.samplesWritten,
                                     av_make_q(1, sc.encCtx->sample_rate),
                                     sc.encCtx->time_base);
    sc.samplesWritten += got;
    *out = sc.convFrame;
    return true;
}

void FFmpegTranscoder::applyCopyPtsOffset(StreamCtx &sc, AVPacket *pkt)
{
    if (!pkt || !sc.outStream)
        return;

    // ==================== 1) 取出输入时间戳 ====================
    // MPEG-PS(VOB) 里的 MPEG-2 视频 B 帧经常只有 dts 没有 pts（FFmpeg 解复用侧明确说明
    // "Cannot compute PTS if not present"），而 MKV/MP4 等封装要求 pts/dts 齐全，
    // 缺失时会直接报 "Can't write packet with unknown timestamp" 并中断整个转码。
    const bool hasTimestamp = (pkt->pts != AV_NOPTS_VALUE) || (pkt->dts != AV_NOPTS_VALUE);
    const int64_t inDts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
    const int64_t inPts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;

    const int64_t maxStep = av_rescale_q(kCopyMaxStepUs, AV_TIME_BASE_Q, sc.outStream->time_base);

    // B 帧的 pts 相对 dts 的重排量；异常（坏包）时退化为 pts == dts
    int64_t reorder = hasTimestamp ? (inPts - inDts) : 0;
    if (reorder <= -maxStep || reorder >= maxStep)
        reorder = 0;

    // ==================== 2) 推进输出时间轴 ====================
    // 用"相邻包的输入时间差"推进，而不是直接平移绝对时间戳：
    // VOB 内部存在 PTS 跳变/回绕（解复用器可能给出 2^33 量级的异常 dts），
    // 一旦用绝对时间戳做基准，一个坏包就会把后面整条时间轴带偏，
    // 表现为输出文件时长暴涨到几十小时。用时间差 + 钳制可自动吸收这类异常。
    int64_t outDts = 0;
    if (sc.copyLastDts == AV_NOPTS_VALUE)
    {
        // 本流首个包：以整条输出时间轴的共同起点重基准。
        // 起点取所有流里最早的 dts（复用器按 dts 输出，首个包即最早），
        // 这样音视频之间的相对偏移（B 帧重排延迟）得以保留；若各流都从 0 开始，
        // 音频会比视频提前一个重排延迟（DVD 上约 40ms），出现轻微口型不同步。
        if (m_copyOriginDts == AV_NOPTS_VALUE)
            m_copyOriginDts = hasTimestamp ? inDts : 0;
        outDts = hasTimestamp ? qMax<int64_t>(0, inDts - m_copyOriginDts) : 0;
        sc.copyLastStep = (pkt->duration > 0 && pkt->duration <= maxStep) ? pkt->duration : 1;
    }
    else
    {
        int64_t step = 0;
        if (hasTimestamp && sc.copyLastInDts != AV_NOPTS_VALUE)
            step = inDts - sc.copyLastInDts;

        if (step <= 0 || step > maxStep)
        {
            // 异常步长（含段首/坏包）：优先用本包时长，其次沿用上一个正常步长
            if (pkt->duration > 0 && pkt->duration <= maxStep)
                step = pkt->duration;
            else
                step = (sc.copyLastStep > 0) ? sc.copyLastStep : 1;
        }

        sc.copyLastStep = step;
        outDts = sc.copyLastDts + step;
    }

    if (hasTimestamp)
        sc.copyLastInDts = inDts;

    pkt->dts = outDts;
    pkt->pts = outDts + reorder;
    sc.copyLastDts = outDts;
}

void FFmpegTranscoder::releaseStream(StreamCtx &sc)
{
    if (sc.swsCtx)
    {
        sws_freeContext(sc.swsCtx);
        sc.swsCtx = nullptr;
    }
    if (sc.swrCtx)
    {
        swr_free(&sc.swrCtx);
        sc.swrCtx = nullptr;
    }
    if (sc.decFrame)
    {
        av_frame_free(&sc.decFrame);
        sc.decFrame = nullptr;
    }
    if (sc.decCtx)
    {
        avcodec_free_context(&sc.decCtx);
        sc.decCtx = nullptr;
    }
    // 编码器上下文与输出流跨分段复用，不在段间释放
    sc.inStream = nullptr;
}

void FFmpegTranscoder::cleanupAll()
{
    releaseStream(m_video);
    releaseStream(m_audio);

    if (m_video.convFrame)
    {
        av_frame_free(&m_video.convFrame);
        m_video.convFrame = nullptr;
    }
    if (m_audio.convFrame)
    {
        av_frame_free(&m_audio.convFrame);
        m_audio.convFrame = nullptr;
    }
    if (m_video.encCtx)
    {
        avcodec_free_context(&m_video.encCtx);
        m_video.encCtx = nullptr;
    }
    if (m_audio.encCtx)
    {
        avcodec_free_context(&m_audio.encCtx);
        m_audio.encCtx = nullptr;
    }
    if (m_inCtx)
    {
        avformat_close_input(&m_inCtx);
        m_inCtx = nullptr;
    }
    if (m_outCtx)
    {
        if (!(m_outCtx->oformat && (m_outCtx->oformat->flags & AVFMT_NOFILE)) && m_outCtx->pb)
            avio_closep(&m_outCtx->pb);
        avformat_free_context(m_outCtx);
        m_outCtx = nullptr;
    }

    m_video = StreamCtx();
    m_audio = StreamCtx();
    m_copyOriginDts = AV_NOPTS_VALUE;
    m_headerWritten = false;
}

void FFmpegTranscoder::reportProgress(int percent)
{
    const int old = m_progress.load();
    if (percent <= old)
        return;
    m_progress.store(percent);
    emit progress(percent);
}

#endif // CAN_USE_FFMPEG
