/*****************************************************************
File:        ffmpeg_transcoder.h
Version:     1.0
Author:      cjx
start date:
Description: 基于FFmpeg实现的媒体转码/封装转换组件（通用跨平台）
    主要功能：
        1. 视频格式互转（VOB/MPEG-PS -> MP4/MKV/AVI/MOV/TS 等）
        2. 支持"仅换封装"（流拷贝，速度快、无损）与"重新编码"两种模式
        3. 支持 DVD 多段 VOB（VTS_01_1/2...）合并转换为单个文件
        4. 视频：自动选择可用编码器（H.264: libx264/h264_mf/... 回退 MPEG-4）
        5. 音频：AAC/AC3/MP3 等（原生编码器优先），同样支持流拷贝
        6. 支持目标分辨率缩放（保持显示比例）、码率设置、进度回调、取消

    技术要点：
        1. 解码 -> sws_scale/swr_convert 转换 -> 编码 -> 交错写入 的标准转码流程
        2. 视频按帧计数、音频按采样计数生成连续时间轴，保证多段合并后同步
        3. 转码在独立线程执行，不阻塞界面

Version history
[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef _FFMPEG_TRANSCODER_H
#define _FFMPEG_TRANSCODER_H

#ifdef CAN_USE_FFMPEG

#include <QSize>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// ==================== 转码参数 ====================
struct TranscodeOption
{
    QStringList inputFiles;             // 输入文件列表（多段按顺序合并，如 DVD 的多个 VOB）
    QString outputFile;                 // 输出文件（按扩展名自动匹配封装格式）

    bool videoCopy = false;             // true：不重新编码视频（仅换封装，无损且快）
    AVCodecID videoCodec = AV_CODEC_ID_H264;    // 目标视频编码（videoCopy 时忽略）
    int videoBitrate = 2000000;         // 视频码率（bps）

    bool audioCopy = false;             // true：不重新编码音频
    AVCodecID audioCodec = AV_CODEC_ID_AAC;     // 目标音频编码（audioCopy 时忽略）
    int audioBitrate = 128000;          // 音频码率（bps）

    int outWidth = 0;                   // 目标宽度（0 表示保持原始分辨率）
    int outHeight = 0;                  // 目标高度（0 表示保持原始分辨率）
    bool keepDisplayAspect = true;      // 缩放时保持显示比例（结合像素宽高比）
};

// ==================== 转码线程 ====================
class FFmpegTranscoder : public QThread
{
    Q_OBJECT
public:
    explicit FFmpegTranscoder(QObject *parent = nullptr);
    ~FFmpegTranscoder() override;

    /** 设置转码参数（必须在 start() 之前调用） */
    void setOption(const TranscodeOption &option);
    /** 请求取消（异步，转码循环检测到后退出） */
    void cancel();

    /** 是否正在取消 */
    bool isCanceled() const { return m_cancel.load(); }

    // ==================== 编码器探测（供界面列出实际可用项） ====================
    /** 查找可用编码器（优先默认实现，其次常见外挂实现，如 H.264 的 libx264/h264_mf） */
    static const AVCodec *findEncoder(AVCodecID codecId);
    /** 指定编码是否可用 */
    static bool isEncoderAvailable(AVCodecID codecId);
    /** 指定编码可用的编码器名称列表 */
    static QStringList availableEncoderNames(AVCodecID codecId);
    /** 目标容器（按文件扩展名）是否可写 */
    static bool isOutputFormatAvailable(const QString &filePath);
    /** 探测视频分辨率（无视频流或打开失败返回 QSize()） */
    static QSize probeVideoResolution(const QString &filePath);

signals:
    /** 进度（0-100） */
    void progress(int percent);
    /** 状态/日志信息 */
    void stateInfo(const QString &info);
    /** 转码结束（success 表示成功，msg 为结果描述）
     *  @note 不使用 finished 命名，避免与 QThread::finished() 产生重载歧义 */
    void transcodeFinished(bool success, const QString &msg);
    /** 错误信息 */
    void errorOccurred(const QString &error);

protected:
    void run() override;

private:
    // 单个流的处理上下文
    struct StreamCtx
    {
        int inIndex = -1;               // 输入流索引
        int outIndex = -1;              // 输出流索引
        AVStream *inStream = nullptr;
        AVStream *outStream = nullptr;
        AVCodecContext *decCtx = nullptr;
        AVCodecContext *encCtx = nullptr;
        SwsContext *swsCtx = nullptr;
        SwrContext *swrCtx = nullptr;
        AVFrame *decFrame = nullptr;    // 解码输出帧
        AVFrame *convFrame = nullptr;   // 转换（缩放/重采样）后的待编码帧
        uint8_t *convBuffer = nullptr;  // 转换帧数据缓冲
        bool enabled = false;           // 该流是否参与输出
        bool copy = false;              // 是否流拷贝
        int outWidth = 0;
        int outHeight = 0;
        int64_t frameIndex = 0;         // 视频帧计数（生成连续PTS）
        int64_t samplesWritten = 0;     // 音频采样计数（生成连续PTS）
        // 流拷贝模式下的时间轴状态（均在输出流时间基下）
        int64_t copyLastDts = AV_NOPTS_VALUE;   // 已写入的最后一个dts（输出时基）
        int64_t copyLastInDts = AV_NOPTS_VALUE; // 上一个包的输入dts（用于推算时间差）
        int64_t copyLastStep = 0;               // 上一个正常的时间步长（异常时兜底复用）
    };

    bool prepareOutput(QString &errorInfo);      // 依据首个输入创建输出上下文与输出流
    bool setupDecoder(StreamCtx &sc, AVMediaType type);
    bool setupEncoder(StreamCtx &sc, AVMediaType type, QString &errorInfo);
    /** 多段合并校验：后续分段的流必须与首段（输出流已固定）编码一致 */
    bool checkSegmentCompatible(StreamCtx &sc, AVMediaType type, QString &errorInfo);
    bool processInput(const QString &path, QString &errorInfo);
    bool handleVideoPacket(StreamCtx &sc, AVPacket *pkt, QString &errorInfo);
    bool handleAudioPacket(StreamCtx &sc, AVPacket *pkt, QString &errorInfo);
    bool writeEncodedPackets(StreamCtx &sc, QString &errorInfo);
    bool flushDecoder(StreamCtx &sc, QString &errorInfo);
    bool flushEncoder(StreamCtx &sc, QString &errorInfo);
    bool convertVideoFrame(StreamCtx &sc, AVFrame *src);
    bool convertAudioFrame(StreamCtx &sc, AVFrame *src, AVFrame **out);
    /** 流拷贝：把输入时间戳映射到连续递增的输出时间轴上（多段拼接、时间戳跳变均可吸收） */
    void applyCopyPtsOffset(StreamCtx &sc, AVPacket *pkt);
    void releaseStream(StreamCtx &sc);
    void cleanupAll();
    void reportProgress(int percent);

    static AVPixelFormat pickPixelFormat(const AVCodec *codec, AVPixelFormat src);
    static enum AVSampleFormat pickSampleFormat(const AVCodec *codec, enum AVSampleFormat src);
    static int pickSampleRate(const AVCodec *codec, int srcRate);

    TranscodeOption m_option;
    AVFormatContext *m_inCtx = nullptr;
    AVFormatContext *m_outCtx = nullptr;
    StreamCtx m_video;
    StreamCtx m_audio;

    std::atomic<bool> m_cancel{false};
    std::atomic<int> m_progress{0};
    // 进度按输入字节数统计：容器 duration 元数据在部分 VOB 上本身就是错的，不可依赖
    qint64 m_totalInputBytes = 0;       // 全部输入文件总字节数
    qint64 m_doneInputBytes = 0;        // 已处理完的输入文件字节数
    int64_t m_skippedPackets = 0;       // 因源数据损坏被跳过的包数（用于结束提示）
    int64_t m_copyOriginDts = AV_NOPTS_VALUE;   // 流拷贝时整条输出时间轴的起点（输出时基）
    bool m_headerWritten = false;
};

#endif // CAN_USE_FFMPEG

#endif // _FFMPEG_TRANSCODER_H
