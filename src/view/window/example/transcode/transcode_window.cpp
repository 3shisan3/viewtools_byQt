#include "transcode_window.h"

#ifdef CAN_USE_FFMPEG

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStringList>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "core/media/dvd_utils.h"
#include "core/transcode/ffmpeg_transcoder.h"

namespace
{
const int kCopyStream = -1;   // 下拉项数据：流拷贝（不重新编码）
}

TranscodeWindow::TranscodeWindow(QWidget *parent)
    : QWidget(parent), m_inputList_(new QListWidget),
      m_addFileBtn_(new QPushButton(tr("add files"))), m_addDvdBtn_(new QPushButton(tr("add dvd folder"))),
      m_removeBtn_(new QPushButton(tr("remove"))), m_clearBtn_(new QPushButton(tr("clear"))),
      m_outputEdit_(new QLineEdit), m_outputBtn_(new QPushButton(tr("browse"))),
      m_formatCbx_(new QComboBox), m_videoCodecCbx_(new QComboBox), m_audioCodecCbx_(new QComboBox),
      m_resolutionCbx_(new QComboBox), m_keepAspectCbx_(new QCheckBox(tr("keep display aspect"))),
      m_videoBitrateSpin_(new QSpinBox), m_audioBitrateCbx_(new QComboBox), m_hintLabel_(new QLabel),
      m_startBtn_(new QPushButton(tr("start"))), m_cancelBtn_(new QPushButton(tr("cancel"))),
      m_progressBar_(new QProgressBar), m_statusLabel_(new QLabel(tr("idle")))
{
    setWindowTitle(tr("Media Transcoder By FFmpeg"));
    resize(720, 520);

    initCodecOptions();
    initWidgetInfo();
    initLayout();
    setupConnections();

    updateOptionStates();    // 初始化控件可用状态与推荐码率提示

    // 编码器可用性提示（FFmpeg 自身不带 H.264 编码器，需外挂 libx264 / MediaFoundation）
    if (!FFmpegTranscoder::isEncoderAvailable(AV_CODEC_ID_H264))
    {
        m_statusLabel_->setText(
            tr("tip: no H.264 encoder in current FFmpeg build, MPEG-4 is used instead. "
               "Enable libx264 (--enable-libx264) or MediaFoundation (--enable-mediafoundation) "
               "when building FFmpeg to get H.264."));
    }
}

TranscodeWindow::~TranscodeWindow()
{
    if (m_transcoder_)
    {
        m_transcoder_->cancel();
        if (m_transcoder_->isRunning())
            m_transcoder_->wait(5000);
    }
}

void TranscodeWindow::initCodecOptions()
{
    // 目标容器
    const char *kFormats[] = {"mp4", "mkv", "avi", "mov", "ts", "mpg", "flv"};
    for (const char *fmt : kFormats)
    {
        m_formatCbx_->addItem(QString::fromUtf8(fmt), QString::fromUtf8(fmt));
    }
    // 标记当前 FFmpeg 构建不支持的容器
    QStandardItemModel *fmtModel = qobject_cast<QStandardItemModel *>(m_formatCbx_->model());
    for (int i = 0; i < m_formatCbx_->count(); ++i)
    {
        const QString fmt = m_formatCbx_->itemData(i).toString();
        if (!FFmpegTranscoder::isOutputFormatAvailable(QString("sample.%1").arg(fmt)) && fmtModel)
        {
            fmtModel->item(i)->setEnabled(false);
            fmtModel->item(i)->setToolTip(tr("muxer not available in current FFmpeg build"));
        }
    }

    // 目标容器：为每个封装格式补充说明
    const char *kFormatTips[] = {
        "mp4: 通用性最好，手机/网页/播放器都支持；但只接受 H.264/MPEG-4 等编码，不能装 MPEG-2(VOB)",
        "mkv: 几乎支持所有编码（含 MPEG-2），兼容性强，推荐用于\"仅换封装\"",
        "avi: 老牌格式，兼容旧设备，功能有限",
        "mov: QuickTime 格式，与 mp4 类似",
        "ts: 流媒体/电视广播格式，支持 MPEG-2",
        "mpg: MPEG-1/2 常用容器，与 DVD 同代",
        "flv: 网页直播/早期视频网站格式",
    };
    for (int i = 0; i < m_formatCbx_->count() && i < static_cast<int>(sizeof(kFormatTips) / sizeof(kFormatTips[0])); ++i)
    {
        m_formatCbx_->setItemData(i, QString::fromUtf8(kFormatTips[i]), Qt::ToolTipRole);
    }
    m_formatCbx_->setToolTip(tr("输出封装格式：mp4 通用性最好；mkv 兼容性最强；转 VOB(MPEG-2) 时选 mkv/ts/mpg"));

    // 视频编码（每项附带中文说明）
    struct CodecItem
    {
        const char *text;
        int id;
        const char *tip;
    };
    const CodecItem kVideoCodecs[] = {
        {"copy stream (remux only, lossless)", kCopyStream,
         "仅换封装(remux)：不重新编码，速度最快、完全无损。\n注意：MP4 容器装不下 VOB 的 MPEG-2 视频，此时请选 mkv/ts/mpg，或改选重新编码"},
        {"H.264 (auto: libx264/h264_mf/...)", AV_CODEC_ID_H264,
         "H.264/AVC：目前通用性最好的高清编码，画质与体积均衡，转 MP4 首选。\n依赖 FFmpeg 编译时启用 libx264 或 MediaFoundation（Windows）"},
        {"MPEG-4", AV_CODEC_ID_MPEG4,
         "MPEG-4 Part2：老牌编码，FFmpeg 自带、任何环境都可用，速度快；压缩率低于 H.264"},
        {"MPEG-2", AV_CODEC_ID_MPEG2VIDEO,
         "MPEG-2：与 DVD 同代的编码，体积较大，一般仅用于输出 mpg/ts 格式"},
    };
    const CodecItem kAudioCodecs[] = {
        {"copy stream (remux only, lossless)", kCopyStream,
         "仅换封装(remux)：音频不重新编码，保持原样、无损"},
        {"AAC", AV_CODEC_ID_AAC,
         "AAC：最通用的音频编码，MP4/MKV 推荐，FFmpeg 自带"},
        {"AC3", AV_CODEC_ID_AC3,
         "AC3(杜比数字)：家庭影院/电视常用，支持多声道"},
        {"MP3", AV_CODEC_ID_MP3,
         "MP3：兼容性最好的音频编码"},
    };

    auto fillCodec = [](QComboBox *cbx, const CodecItem *items, int count) {
        for (int i = 0; i < count; ++i)
        {
            cbx->addItem(QString::fromUtf8(items[i].text), items[i].id);
            cbx->setItemData(i, QString::fromUtf8(items[i].tip), Qt::ToolTipRole);
        }

        QStandardItemModel *model = qobject_cast<QStandardItemModel *>(cbx->model());
        for (int i = 0; i < cbx->count(); ++i)
        {
            const int id = cbx->itemData(i).toInt();
            if (id == kCopyStream)
                continue;
            if (!FFmpegTranscoder::isEncoderAvailable(static_cast<AVCodecID>(id)) && model)
            {
                model->item(i)->setEnabled(false);
                model->item(i)->setToolTip(QObject::tr("当前 FFmpeg 未编译该编码器（如需 H.264 请启用 libx264 或 MediaFoundation 后重新编译 FFmpeg）"));
            }
        }
        // 首个可用项作为默认选择
        for (int i = 0; i < cbx->count(); ++i)
        {
            bool enabled = true;
            if (model)
                enabled = model->item(i)->isEnabled();
            if (enabled)
            {
                cbx->setCurrentIndex(i);
                break;
            }
        }
    };
    // 使用 sizeof 计算数组长度（避免依赖 C++17 的 std::size，兼容 C++11/14 编译器）
    fillCodec(m_videoCodecCbx_, kVideoCodecs, static_cast<int>(sizeof(kVideoCodecs) / sizeof(kVideoCodecs[0])));
    fillCodec(m_audioCodecCbx_, kAudioCodecs, static_cast<int>(sizeof(kAudioCodecs) / sizeof(kAudioCodecs[0])));

    // 目标分辨率（QSize(0,0) 表示保持原始）
    m_resolutionCbx_->addItem(tr("keep original"), QSize(0, 0));
    m_resolutionCbx_->addItem("1920x1080", QSize(1920, 1080));
    m_resolutionCbx_->addItem("1280x720", QSize(1280, 720));
    m_resolutionCbx_->addItem("720x576 (DVD PAL)", QSize(720, 576));
    m_resolutionCbx_->addItem("720x480 (DVD NTSC)", QSize(720, 480));
    m_resolutionCbx_->addItem("640x480", QSize(640, 480));

    // 音频码率
    m_audioBitrateCbx_->addItem("128 kbps", 128);
    m_audioBitrateCbx_->addItem("192 kbps", 192);
    m_audioBitrateCbx_->addItem("256 kbps", 256);
    m_audioBitrateCbx_->setCurrentIndex(0);
}

void TranscodeWindow::initWidgetInfo()
{
    m_keepAspectCbx_->setChecked(true);
    m_keepAspectCbx_->setToolTip(tr("保持真实显示比例：DVD 的 720x480/720x576 会按 4:3 或 16:9 显示（依据像素宽高比），避免画面拉伸变形"));

    m_videoBitrateSpin_->setRange(100, 50000);
    m_videoBitrateSpin_->setSuffix(" kbps");
    m_videoBitrateSpin_->setValue(2000);
    m_videoBitrateSpin_->setToolTip(tr("视频码率：数值越大画质越好、文件也越大。\n参考值：480p 约 1500-2500，720p 约 3000-5000，1080p 约 6000-10000"));
    m_resolutionCbx_->setToolTip(tr("目标分辨率：keep original 表示保持原始尺寸；缩小可减小文件体积，放大不会提升清晰度"));
    m_audioBitrateCbx_->setToolTip(tr("音频码率：128kbps 已能满足大多数场景，音乐类可选 192/256"));
    m_outputEdit_->setToolTip(tr("输出文件路径，扩展名决定封装格式（也可在右侧 container 下拉框切换）"));
    m_startBtn_->setToolTip(tr("开始转码；多个输入文件将按顺序合并为一个输出文件"));
    m_cancelBtn_->setToolTip(tr("取消当前转码（自动删除未完成的输出文件）"));

    m_progressBar_->setRange(0, 100);
    m_progressBar_->setValue(0);
    m_cancelBtn_->setEnabled(false);

    m_inputList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_outputEdit_->setPlaceholderText(tr("output file path (extension decides the container)"));
    m_statusLabel_->setWordWrap(true);
}

void TranscodeWindow::initLayout()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // 输入区
    mainLayout->addWidget(new QLabel(tr("input files (processed in order, multiple VOBs will be merged):")));
    mainLayout->addWidget(m_inputList_, 1);

    QHBoxLayout *inputBtnLayout = new QHBoxLayout;
    inputBtnLayout->addWidget(m_addFileBtn_);
    inputBtnLayout->addWidget(m_addDvdBtn_);
    inputBtnLayout->addWidget(m_removeBtn_);
    inputBtnLayout->addWidget(m_clearBtn_);
    inputBtnLayout->addStretch();
    mainLayout->addLayout(inputBtnLayout);

    // 输出区
    QHBoxLayout *outputLayout = new QHBoxLayout;
    outputLayout->addWidget(new QLabel(tr("output:")));
    outputLayout->addWidget(m_outputEdit_, 1);
    outputLayout->addWidget(m_outputBtn_);
    outputLayout->addWidget(new QLabel(tr("container:")));
    outputLayout->addWidget(m_formatCbx_);
    mainLayout->addLayout(outputLayout);

    // 编码参数
    QGridLayout *paramLayout = new QGridLayout;
    paramLayout->addWidget(new QLabel(tr("video codec:")), 0, 0);
    paramLayout->addWidget(m_videoCodecCbx_, 0, 1);
    paramLayout->addWidget(new QLabel(tr("audio codec:")), 0, 2);
    paramLayout->addWidget(m_audioCodecCbx_, 0, 3);
    paramLayout->addWidget(new QLabel(tr("resolution:")), 1, 0);
    paramLayout->addWidget(m_resolutionCbx_, 1, 1);
    paramLayout->addWidget(m_keepAspectCbx_, 1, 2, 1, 2);
    paramLayout->addWidget(new QLabel(tr("video bitrate:")), 2, 0);
    paramLayout->addWidget(m_videoBitrateSpin_, 2, 1);
    paramLayout->addWidget(new QLabel(tr("audio bitrate:")), 2, 2);
    paramLayout->addWidget(m_audioBitrateCbx_, 2, 3);
    mainLayout->addLayout(paramLayout);

    // 控制区
    QHBoxLayout *controlLayout = new QHBoxLayout;
    controlLayout->addWidget(m_startBtn_);
    controlLayout->addWidget(m_cancelBtn_);
    controlLayout->addStretch();
    mainLayout->addLayout(controlLayout);

    mainLayout->addWidget(m_progressBar_);
    mainLayout->addWidget(m_statusLabel_);
    m_hintLabel_->setWordWrap(true);
    mainLayout->addWidget(m_hintLabel_);
}

void TranscodeWindow::setupConnections()
{
    connect(m_addFileBtn_, &QPushButton::clicked, this, &TranscodeWindow::addInputFiles);
    connect(m_addDvdBtn_, &QPushButton::clicked, this, &TranscodeWindow::addDvdFolder);
    connect(m_removeBtn_, &QPushButton::clicked, this, &TranscodeWindow::removeSelectedInput);
    connect(m_clearBtn_, &QPushButton::clicked, this, &TranscodeWindow::clearInputs);
    connect(m_outputBtn_, &QPushButton::clicked, this, &TranscodeWindow::selectOutputFile);
    connect(m_formatCbx_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TranscodeWindow::onOutputFormatChanged);
    connect(m_videoCodecCbx_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TranscodeWindow::onCodecOptionChanged);
    connect(m_audioCodecCbx_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TranscodeWindow::onCodecOptionChanged);
    connect(m_resolutionCbx_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TranscodeWindow::onCodecOptionChanged);
    // 用户手动改过码率后不再自动覆盖推荐值
    connect(m_videoBitrateSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { m_bitrateEdited_ = true; });
    connect(m_startBtn_, &QPushButton::clicked, this, &TranscodeWindow::startTranscode);
    connect(m_cancelBtn_, &QPushButton::clicked, this, &TranscodeWindow::cancelTranscode);
}

// ==================== 输入输出 ====================

void TranscodeWindow::presetInput(const QStringList &files)
{
    if (files.isEmpty())
        return;

    clearInputs();
    for (const QString &file : files)
    {
        m_inputFiles_ << file;
        m_inputList_->addItem(QDir::toNativeSeparators(file));
    }
    m_outputEdit_->setText(suggestedOutputPath());
    probeSourceResolution();
    updateOptionStates();
}

void TranscodeWindow::addInputFiles()
{
    const QString startDir = m_lastOpenDir_.isEmpty()
                                 ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
                                 : m_lastOpenDir_;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("select media files"), startDir,
        tr("Video Files (*.vob *.mp4 *.mkv *.avi *.mpg *.mpeg *.ts *.m2ts *.mov *.wmv *.flv);;All Files (*.*)"));
    if (files.isEmpty())
        return;

    m_lastOpenDir_ = QFileInfo(files.first()).absolutePath();
    for (const QString &file : files)
    {
        if (m_inputFiles_.contains(file))
            continue;
        m_inputFiles_ << file;
        m_inputList_->addItem(QDir::toNativeSeparators(file));
    }

    if (m_outputEdit_->text().trimmed().isEmpty())
        m_outputEdit_->setText(suggestedOutputPath());
}

void TranscodeWindow::addDvdFolder()
{
    const QString startDir = m_lastOpenDir_.isEmpty()
                                 ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
                                 : m_lastOpenDir_;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("select DVD / VIDEO_TS folder"), startDir);
    if (dir.isEmpty())
        return;

    m_lastOpenDir_ = dir;
    // 跳过菜单/引导文件，按正片顺序加入（若只有菜单文件则全部加入）
    QStringList vobs = DvdUtils::collectVobSequence(dir, true);
    if (vobs.isEmpty())
        vobs = DvdUtils::collectVobSequence(dir, false);
    if (vobs.isEmpty())
    {
        QMessageBox::warning(this, tr("warning"), tr("No VOB file found in folder:\n%1").arg(dir));
        return;
    }

    for (const QString &file : vobs)
    {
        if (m_inputFiles_.contains(file))
            continue;
        m_inputFiles_ << file;
        m_inputList_->addItem(QDir::toNativeSeparators(file));
    }

    if (m_outputEdit_->text().trimmed().isEmpty())
        m_outputEdit_->setText(suggestedOutputPath());

    probeSourceResolution();
    updateOptionStates();
}

void TranscodeWindow::removeSelectedInput()
{
    const QList<QListWidgetItem *> selected = m_inputList_->selectedItems();
    for (QListWidgetItem *item : selected)
    {
        const int row = m_inputList_->row(item);
        m_inputFiles_.removeAt(row);
        delete m_inputList_->takeItem(row);
    }
}

void TranscodeWindow::clearInputs()
{
    m_inputFiles_.clear();
    m_inputList_->clear();
    m_sourceWidth_ = 0;
    m_sourceHeight_ = 0;
    updateOptionStates();
}

void TranscodeWindow::selectOutputFile()
{
    const QString format = m_formatCbx_->currentData().toString();
    const QString startPath = m_outputEdit_->text().trimmed().isEmpty()
                                  ? suggestedOutputPath()
                                  : m_outputEdit_->text().trimmed();
    const QString file = QFileDialog::getSaveFileName(
        this, tr("select output file"), startPath,
        tr("%1 Files (*.%2);;All Files (*.*)").arg(format.toUpper()).arg(format));
    if (!file.isEmpty())
        m_outputEdit_->setText(QDir::toNativeSeparators(file));
}

void TranscodeWindow::onOutputFormatChanged(int index)
{
    Q_UNUSED(index)
    updateOutputSuffix();
}

void TranscodeWindow::updateOutputSuffix()
{
    const QString text = m_outputEdit_->text().trimmed();
    if (text.isEmpty())
        return;

    const QString format = m_formatCbx_->currentData().toString();
    QFileInfo info(text);
    m_outputEdit_->setText(QDir::toNativeSeparators(info.path() + "/" + info.completeBaseName() + "." + format));
}

void TranscodeWindow::probeSourceResolution()
{
    m_sourceWidth_ = 0;
    m_sourceHeight_ = 0;
    if (m_inputFiles_.isEmpty())
        return;

    const QSize size = FFmpegTranscoder::probeVideoResolution(m_inputFiles_.first());
    m_sourceWidth_ = size.width();
    m_sourceHeight_ = size.height();
}

int TranscodeWindow::recommendedBitrate(int height) const
{
    if (height >= 1600) return 25000;   // 4K(2160p) 及以上
    if (height >= 1200) return 12000;   // QHD/2K(1440p)
    if (height >= 900)  return 6000;    // 1080p
    if (height >= 600)  return 3500;    // 720p
    if (height >= 480)  return 2000;    // 480p / DVD
    return 1200;                        // 更低分辨率
}

QString TranscodeWindow::bitrateHintFor(int height) const
{
    if (height >= 1600) return tr("4K/2160p 常用 16000-40000 kbps");
    if (height >= 1200) return tr("2K/1440p 常用 8000-18000 kbps");
    if (height >= 900)  return tr("1080p 常用 4000-9000 kbps");
    if (height >= 600)  return tr("720p 常用 2500-5000 kbps");
    if (height >= 480)  return tr("480p/DVD 常用 1000-2500 kbps");
    return tr("低分辨率常用 600-1500 kbps");
}

void TranscodeWindow::onCodecOptionChanged()
{
    updateOptionStates();
}

void TranscodeWindow::updateOptionStates()
{
    const bool videoCopy = (m_videoCodecCbx_->currentData().toInt() == kCopyStream);
    const bool audioCopy = (m_audioCodecCbx_->currentData().toInt() == kCopyStream);

    // 仅换封装（copy stream）时码率/分辨率都不参与处理，自动置灰避免误解
    m_videoBitrateSpin_->setEnabled(!videoCopy);
    m_resolutionCbx_->setEnabled(!videoCopy);
    m_keepAspectCbx_->setEnabled(!videoCopy);
    m_audioBitrateCbx_->setEnabled(!audioCopy);

    // 目标高度：优先用户选择，否则取源视频分辨率
    const QSize target = m_resolutionCbx_->currentData().toSize();
    const int targetH = (target.height() > 0) ? target.height() : qMax(m_sourceHeight_, 480);
    const int reco = recommendedBitrate(targetH);

    const QString reasonDisable = tr("copy stream 模式：不重新编码，码率与分辨率沿用原始码流（无需设置、也无法修改）");
    m_videoBitrateSpin_->setToolTip(videoCopy ? reasonDisable
                                              : tr("视频码率：数值越大画质越好、文件越大。%1").arg(bitrateHintFor(targetH)));
    const QString resDisable = tr("copy stream 模式：不重新编码，无法改变分辨率");
    m_resolutionCbx_->setToolTip(videoCopy ? resDisable
                                           : tr("目标分辨率：keep original 保持原始尺寸；缩小可减小体积，放大不会提升清晰度"));
    m_audioBitrateCbx_->setToolTip(audioCopy ? tr("copy stream 模式：音频保持原始码率（无损）")
                                             : tr("音频码率：128kbps 已能满足大多数场景，音乐类可选 192/256"));

    // 未手动改过时自动填入推荐值
    if (!m_bitrateEdited_ && !videoCopy)
    {
        QSignalBlocker blocker(m_videoBitrateSpin_);
        m_videoBitrateSpin_->setValue(reco);
    }

    // 底部提示：说明当前模式与推荐值
    if (videoCopy && audioCopy)
    {
        m_hintLabel_->setText(tr("当前为「仅换封装」：视频/音频都不重新编码，保留原始画质与码率，速度最快、无损。"
                                 "（注意：MP4 装不下 VOB 的 MPEG-2，此时请选 mkv/ts/mpg）"));
    }
    else
    {
        const QString sourceInfo = (m_sourceWidth_ > 0 && m_sourceHeight_ > 0)
                                       ? tr("，源文件 %1x%2").arg(m_sourceWidth_).arg(m_sourceHeight_)
                                       : QString();
        m_hintLabel_->setText(tr("推荐视频码率 %1 kbps（目标高度 %2p%3）。%4")
                                  .arg(reco).arg(targetH).arg(sourceInfo).arg(bitrateHintFor(targetH)));
    }
}

QString TranscodeWindow::suggestedOutputPath() const
{
    if (m_inputFiles_.isEmpty())
        return QString();

    const QFileInfo info(m_inputFiles_.first());
    // 多文件合并时以所在目录命名，单文件沿用原文件名
    const QString baseName = (m_inputFiles_.size() > 1) ? info.dir().dirName() : info.completeBaseName();
    return QDir::toNativeSeparators(info.path() + "/" + baseName + "." + m_formatCbx_->currentData().toString());
}

// ==================== 转码控制 ====================

void TranscodeWindow::startTranscode()
{
    if (m_inputFiles_.isEmpty())
    {
        QMessageBox::warning(this, tr("warning"), tr("Please add at least one input file."));
        return;
    }

    QString output = m_outputEdit_->text().trimmed();
    if (output.isEmpty())
    {
        output = suggestedOutputPath();
        if (output.isEmpty())
        {
            QMessageBox::warning(this, tr("warning"), tr("Please set the output file path."));
            return;
        }
        m_outputEdit_->setText(output);
    }
    updateOutputSuffix();
    output = QDir::fromNativeSeparators(m_outputEdit_->text().trimmed());

    if (QFileInfo::exists(output))
    {
        if (QMessageBox::question(this, tr("confirm"),
                                  tr("Output file already exists, overwrite it?\n%1").arg(output))
            != QMessageBox::Yes)
        {
            return;
        }
    }

    // 组装转码参数
    TranscodeOption option;
    option.inputFiles = m_inputFiles_;
    option.outputFile = output;

    const int videoCodec = m_videoCodecCbx_->currentData().toInt();
    option.videoCopy = (videoCodec == kCopyStream);
    option.videoCodec = option.videoCopy ? AV_CODEC_ID_NONE : static_cast<AVCodecID>(videoCodec);

    const int audioCodec = m_audioCodecCbx_->currentData().toInt();
    option.audioCopy = (audioCodec == kCopyStream);
    option.audioCodec = option.audioCopy ? AV_CODEC_ID_NONE : static_cast<AVCodecID>(audioCodec);

    const QSize targetSize = m_resolutionCbx_->currentData().toSize();
    option.outWidth = targetSize.width();
    option.outHeight = targetSize.height();
    option.keepDisplayAspect = m_keepAspectCbx_->isChecked();
    option.videoBitrate = m_videoBitrateSpin_->value() * 1000;
    option.audioBitrate = m_audioBitrateCbx_->currentData().toInt() * 1000;

    // 启动转码线程
    m_transcoder_ = new FFmpegTranscoder(this);
    connect(m_transcoder_, &FFmpegTranscoder::progress, this, &TranscodeWindow::onProgress);
    connect(m_transcoder_, &FFmpegTranscoder::stateInfo, this, &TranscodeWindow::onStateInfo);
    connect(m_transcoder_, &FFmpegTranscoder::transcodeFinished, this, &TranscodeWindow::onFinished);
    connect(m_transcoder_, &FFmpegTranscoder::errorOccurred, this, &TranscodeWindow::onError);
    m_transcoder_->setOption(option);
    m_transcoder_->start();

    m_progressBar_->setValue(0);
    updateControlState(true);
    m_statusLabel_->setText(tr("transcoding..."));
}

void TranscodeWindow::cancelTranscode()
{
    if (!m_transcoder_)
        return;
    m_transcoder_->cancel();
    m_statusLabel_->setText(tr("canceling..."));
    m_cancelBtn_->setEnabled(false);
}

void TranscodeWindow::onProgress(int percent)
{
    m_progressBar_->setValue(percent);
}

void TranscodeWindow::onStateInfo(const QString &info)
{
    m_statusLabel_->setText(info);
}

void TranscodeWindow::onFinished(bool success, const QString &msg)
{
    updateControlState(false);
    m_statusLabel_->setText(msg);

    if (m_transcoder_)
    {
        m_transcoder_->deleteLater();
        m_transcoder_ = nullptr;
    }

    if (success)
    {
        m_progressBar_->setValue(100);
        QMessageBox::information(this, tr("finished"), tr("Transcode finished:\n%1").arg(msg));
    }
}

void TranscodeWindow::onError(const QString &error)
{
    updateControlState(false);
    m_statusLabel_->setText(tr("error: %1").arg(error));
    QMessageBox::critical(this, tr("error"), error);

    if (m_transcoder_)
    {
        m_transcoder_->deleteLater();
        m_transcoder_ = nullptr;
    }
}

void TranscodeWindow::updateControlState(bool running)
{
    m_running_ = running;
    m_startBtn_->setEnabled(!running);
    m_cancelBtn_->setEnabled(running);
    m_addFileBtn_->setEnabled(!running);
    m_addDvdBtn_->setEnabled(!running);
    m_removeBtn_->setEnabled(!running);
    m_clearBtn_->setEnabled(!running);
    m_outputBtn_->setEnabled(!running);
}

void TranscodeWindow::closeEvent(QCloseEvent *event)
{
    if (m_running_ && m_transcoder_)
    {
        if (QMessageBox::question(this, tr("confirm"),
                                  tr("Transcoding is in progress, cancel and close?"))
            != QMessageBox::Yes)
        {
            event->ignore();
            return;
        }
        m_transcoder_->cancel();
        if (m_transcoder_->isRunning())
            m_transcoder_->wait(5000);
    }
    QWidget::closeEvent(event);
}

#endif // CAN_USE_FFMPEG
