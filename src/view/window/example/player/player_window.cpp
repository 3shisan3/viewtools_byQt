#include "player_window.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QStandardPaths>
#include <QString>
#include <QStyle>
#include <QTime>
#include <QTimer>
#include <QToolTip>
#include <QCursor>
#include <QVBoxLayout>

#include "core/media/dvd_utils.h"
#include "utils/option.h"
#include "view/widget/player/base_player_widget.h"
#include "view/widget/player/ffmpeg_player_widget.h"
#include "view/widget/player/qmedia_player_widget.h"
#ifdef CAN_USE_FFMPEG
#include "view/window/example/transcode/transcode_window.h"
#endif

static QString formatTime(qint64 milliseconds)
{
    QTime time((milliseconds / 3600000) % 24,
               (milliseconds / 60000) % 60,
               (milliseconds / 1000) % 60);
    return milliseconds >= 3600000 ? time.toString("hh:mm:ss") : time.toString("mm:ss");
}

PlayerWindow::PlayerWindow(QWidget *parent, PlayerWidget type)
    : QWidget(parent), m_loadFileBtn_(new QPushButton(tr("open file"))), m_openDvdBtn_(new QPushButton(tr("open dvd folder"))), m_fitBtn_(new QPushButton(tr("fit"))), m_fullScreenBtn_(new QPushButton(tr("fullscreen"))), m_urlInput_(new QLineEdit), m_loadUrlBtn_(new QPushButton(tr("open url"))), m_playBtn_(new QPushButton), m_progressSlider_(new QSlider(Qt::Horizontal)), m_volumeBtn_(new QPushButton), m_volumeSlider_(new QSlider(Qt::Horizontal)), m_timeInfo_(new QLabel("00:00 / 00:00")), m_segmentRow_(new QWidget(this)), m_prevSegBtn_(new QPushButton(tr("prev"))), m_segmentCbx_(new QComboBox), m_nextSegBtn_(new QPushButton(tr("next"))), m_autoNextCbx_(new QCheckBox(tr("auto next segment"))), m_viewRow_(new QWidget(this)), m_scaleModeCbx_(new QComboBox), m_aspectCbx_(new QComboBox), m_zoomLabel_(new QLabel("100%")), m_zoomOutBtn_(new QPushButton("-")), m_zoomInBtn_(new QPushButton("+")), m_resetViewBtn_(new QPushButton(tr("reset"))), m_lockAspectCbx_(new QCheckBox(tr("lock aspect"))), m_duration_("00:00"), m_dvdIndex_(-1), m_playState_(PlayerWidgetBase::StoppedState), m_lastMediaIsUrl_(false), m_videoAspect_(0.0), m_windowFitted_(false), m_applyingAspectLock_(false)
{
    // 通过type m_playerWidget_指向不同的子类widget
    m_ffmpegPlayer_ = nullptr;
    switch (type)
    {
    case PlayerWidget::BY_QMEDIA:
    {
        m_playerWidget_ = new SsQMediaPlayer(this);
        m_playerInstance_ = static_cast<SsQMediaPlayer *>(m_playerWidget_)->PlayerCore(); // 获取核心类
        // 设置窗口标题
        setWindowTitle(tr("Video Player By QMediaPlayer"));
    }
        break;
#ifdef CAN_USE_FFMPEG
    case PlayerWidget::BY_FFMPEG:
    {
        FFmpegPlayer *ffmpegPlayer = new FFmpegPlayer(this);
        m_playerWidget_ = ffmpegPlayer;
        m_ffmpegPlayer_ = ffmpegPlayer;
        m_playerInstance_ = ffmpegPlayer->PlayerCore(); // 获取核心类
        setWindowTitle(tr("Video Player By FFmpeg"));
    }
        break;
#endif
    default:
        break;
    }

    // 记录基础窗口标题（切换段落时在其上拼接段落信息）
    m_baseTitle_ = windowTitle();

    // 允许自由缩放窗口（不再被视频原始尺寸/推荐尺寸卡住）
    setMinimumSize(320, 240);
    // 关闭即销毁：否则窗口对象（连同内部的解码线程与已打开的媒体文件）会一直驻留
    setAttribute(Qt::WA_DeleteOnClose, true);
    if (m_playerWidget_)
        m_playerWidget_->installEventFilter(this); // 视频区双击全屏

    // 初始化控件信息
    initWidgetInfo();
    // 初始化布局
    initLayout();
    // 连接信号槽
    setupConnections();
}

PlayerWindow::~PlayerWindow()
{
    // 资源通过Qt父子关系自动释放
}

void PlayerWindow::initWidgetInfo()
{
    // 暂将图标设置为qt默认(后续可导入资源)
    m_loadFileBtn_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    m_openDvdBtn_->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    m_openDvdBtn_->setToolTip(tr("load all VOB segments in a DVD/VIDEO_TS folder and play in sequence"));
    m_fitBtn_->setToolTip(tr("resize window to match the video display aspect ratio"));
    m_fullScreenBtn_->setToolTip(tr("toggle fullscreen (F11)"));
    m_playBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_volumeBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));

#ifdef CAN_USE_FFMPEG
    // 格式转换入口（打开转码窗口并预填当前媒体）
    m_convertBtn_ = new QPushButton(tr("convert"));
    m_convertBtn_->setToolTip(tr("convert current media to another format (e.g. VOB to MP4)"));
#endif

    // 段落导航控件初始化
    m_prevSegBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    m_nextSegBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    m_prevSegBtn_->setToolTip(tr("previous segment"));
    m_nextSegBtn_->setToolTip(tr("next segment"));
    m_segmentCbx_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_segmentCbx_->setMinimumContentsLength(18);
    m_segmentCbx_->setToolTip(tr("current DVD segment, click to switch"));
    m_autoNextCbx_->setChecked(true); // 默认开启自动连播
    m_autoNextCbx_->setToolTip(tr("play next segment automatically when current one ends"));

    // ==================== 画面缩放/比例控件 ====================
    // 缩放模式：参考 mpv/VLC 的 video-fit 语义，覆盖"留黑边 / 裁掉超出 / 拉伸 / 原始像素"四类需求
    struct ScaleModeItem
    {
        const char *text;
        VideoScaleMode mode;
        const char *tip;
    };
    const ScaleModeItem kScaleModes[] = {
        {"fit window", VideoScaleMode::FitWindow,
         "keep the whole picture visible, black bars are added when the aspect differs"},
        {"fill window", VideoScaleMode::FillWindow,
         "keep the aspect and crop the parts outside the window (no black bars)"},
        {"stretch", VideoScaleMode::StretchToWindow,
         "fill the window ignoring the aspect ratio (picture may be distorted)"},
        {"original size", VideoScaleMode::OriginalSize,
         "show at 1:1 pixel size; use the wheel to zoom and drag to pan"},
    };
    for (const ScaleModeItem &item : kScaleModes)
    {
        m_scaleModeCbx_->addItem(tr(item.text), static_cast<int>(item.mode));
        m_scaleModeCbx_->setItemData(m_scaleModeCbx_->count() - 1, tr(item.tip), Qt::ToolTipRole);
    }
    m_scaleModeCbx_->setToolTip(tr("how the picture is mapped into the window"));

    // 显示比例：自动按像素宽高比(SAR)，也可强制成常见比例
    m_aspectCbx_->addItem(tr("auto (by SAR)"), 0.0);
    m_aspectCbx_->addItem("16:9", 16.0 / 9.0);
    m_aspectCbx_->addItem("4:3", 4.0 / 3.0);
    m_aspectCbx_->addItem("16:10", 16.0 / 10.0);
    m_aspectCbx_->addItem("21:9", 21.0 / 9.0);
    m_aspectCbx_->addItem("1:1", 1.0);
    m_aspectCbx_->setToolTip(tr("display aspect ratio: auto follows the video pixel aspect ratio (SAR)"));

    m_zoomOutBtn_->setToolTip(tr("zoom out (mouse wheel down)"));
    m_zoomInBtn_->setToolTip(tr("zoom in (mouse wheel up)"));
    m_resetViewBtn_->setToolTip(tr("reset zoom and pan"));
    m_zoomLabel_->setAlignment(Qt::AlignCenter);
    m_zoomLabel_->setMinimumWidth(48);
    m_zoomLabel_->setToolTip(tr("current zoom; drag the picture to pan when zoomed in"));
    m_lockAspectCbx_->setToolTip(tr("keep the window at the video aspect ratio while resizing"));

    // 设置提示信息
    m_urlInput_->setPlaceholderText(tr("enter RTSP url"));

    // 进度条相关内容，设置范围
    m_progressSlider_->setRange(0, 0);
    m_volumeSlider_->setRange(0, 100);
    m_volumeSlider_->setValue(33); // 初始值
}

void PlayerWindow::initLayout()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    // 视频区域占据所有可扩展空间（stretch=1），其余控制控件保持各自推荐高度
    mainLayout->addWidget(m_playerWidget_, 1);

    // 进度行：进度条与时间标签成组（全宽滑块只有进度条一个，与音量条明确区分）
    QHBoxLayout *progressLayout = new QHBoxLayout;
    progressLayout->addWidget(m_progressSlider_, 1);
    m_timeInfo_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_timeInfo_->setFixedWidth(110);
    progressLayout->addWidget(m_timeInfo_);

    QHBoxLayout *controlLayout = new QHBoxLayout;
    controlLayout->addWidget(m_loadFileBtn_);
    controlLayout->addWidget(m_openDvdBtn_);
    controlLayout->addWidget(m_loadUrlBtn_);
    controlLayout->addWidget(m_playBtn_);
    controlLayout->addWidget(m_fitBtn_);
    controlLayout->addWidget(m_fullScreenBtn_);
#ifdef CAN_USE_FFMPEG
    if (m_convertBtn_)
        controlLayout->addWidget(m_convertBtn_);
#endif
    controlLayout->addStretch();

    // 音量：仅喇叭图标 + 固定宽度小滑块（与全宽的进度条明显区分）
    m_volumeSlider_->setFixedWidth(90);
    m_volumeSlider_->setToolTip(tr("volume"));
    controlLayout->addWidget(m_volumeBtn_);
    controlLayout->addWidget(m_volumeSlider_);

    // DVD 段落导航行：默认隐藏，载入 DVD 目录后显示
    QHBoxLayout *segmentLayout = new QHBoxLayout(m_segmentRow_);
    segmentLayout->setContentsMargins(0, 0, 0, 0);
    segmentLayout->addWidget(m_prevSegBtn_);
    segmentLayout->addWidget(m_segmentCbx_, 1);
    segmentLayout->addWidget(m_nextSegBtn_);
    segmentLayout->addWidget(m_autoNextCbx_);
    m_segmentRow_->setVisible(false);

    // 画面控制行：缩放模式 / 显示比例 / 倍率 / 复位 / 锁定窗口比例
    QHBoxLayout *viewLayout = new QHBoxLayout(m_viewRow_);
    viewLayout->setContentsMargins(0, 0, 0, 0);
    viewLayout->addWidget(new QLabel(tr("scale:")));
    viewLayout->addWidget(m_scaleModeCbx_);
    viewLayout->addWidget(new QLabel(tr("aspect:")));
    viewLayout->addWidget(m_aspectCbx_);
    viewLayout->addWidget(m_zoomOutBtn_);
    viewLayout->addWidget(m_zoomLabel_);
    viewLayout->addWidget(m_zoomInBtn_);
    viewLayout->addWidget(m_resetViewBtn_);
    viewLayout->addWidget(m_lockAspectCbx_);
    viewLayout->addStretch();

    mainLayout->addLayout(progressLayout);
    mainLayout->addWidget(m_urlInput_);
    mainLayout->addLayout(controlLayout);
    mainLayout->addWidget(m_viewRow_);
    mainLayout->addWidget(m_segmentRow_);
}

void PlayerWindow::setupConnections()
{
    // 连接内部信号槽
    connect(m_loadFileBtn_, &QPushButton::clicked, this, &PlayerWindow::openFileDialog);
    connect(m_openDvdBtn_, &QPushButton::clicked, this, &PlayerWindow::openDvdFolder);
    connect(m_loadUrlBtn_, &QPushButton::clicked, this, &PlayerWindow::triggerLoadUrl);
    connect(m_prevSegBtn_, &QPushButton::clicked, this, &PlayerWindow::goPrevSegment);
    connect(m_nextSegBtn_, &QPushButton::clicked, this, &PlayerWindow::goNextSegment);
    connect(m_fitBtn_, &QPushButton::clicked, this, &PlayerWindow::fitWindowToVideo);
    connect(m_fullScreenBtn_, &QPushButton::clicked, this, &PlayerWindow::toggleFullScreen);
    connect(m_segmentCbx_, QOverload<int>::of(&QComboBox::activated),
            this, &PlayerWindow::onSegmentActivated);

    // 转发播放器控件的信号
    connect(m_progressSlider_, &QSlider::sliderMoved, m_playerInstance_, &PlayerWidgetBase::seekPlay);
    connect(m_volumeSlider_, &QSlider::sliderMoved, m_playerInstance_, &PlayerWidgetBase::setVolume);
    // 音量数值提示：拖动时在手柄上方显示气泡，并同步到按钮/滑块的悬浮提示
    connect(m_volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        const QString tip = tr("volume: %1%").arg(value);
        m_volumeSlider_->setToolTip(tip);
        m_volumeBtn_->setToolTip(tip);
        if (m_volumeSlider_->isSliderDown())
            QToolTip::showText(QCursor::pos(), tip, m_volumeSlider_);
    });
    // 播放按钮由窗口统一处理：停止态点击时重新播放（当前段落或上次媒体）
    connect(m_playBtn_, &QPushButton::clicked, this, &PlayerWindow::onPlayBtnClicked);
    connect(m_volumeBtn_, &QPushButton::clicked, m_playerInstance_, &PlayerWidgetBase::changeMuteState);
#ifdef CAN_USE_FFMPEG
    if (m_convertBtn_)
        connect(m_convertBtn_, &QPushButton::clicked, this, &PlayerWindow::openTranscodeWindow);
#endif

    // 连接播放器的信号到槽函数
    connect(m_playerInstance_, &PlayerWidgetBase::currentDuration, this, &PlayerWindow::updateTimeDuration);
    connect(m_playerInstance_, &PlayerWidgetBase::currentPosition, this, &PlayerWindow::updateTimePosition);
    connect(m_playerInstance_, &PlayerWidgetBase::playStateChanged, this, &PlayerWindow::updatePlayBtnState);
    connect(m_playerInstance_, &PlayerWidgetBase::currentVolume, this, &PlayerWindow::updateVolumeSlider);
    connect(m_playerInstance_, &PlayerWidgetBase::muteStateChanged, this, &PlayerWindow::updateVolumeBtnState);

    // 自然播放结束（用于 DVD 段落自动连播）
    connect(m_playerInstance_, &PlayerWidgetBase::mediaFinished, this, &PlayerWindow::onMediaFinished);
    // 视频画面几何（用于按显示比例渲染/适配窗口）
    connect(m_playerInstance_, &PlayerWidgetBase::videoGeometryChanged,
            this, &PlayerWindow::onVideoGeometry);

    // ==================== 画面缩放/比例控制 ====================
    connect(m_scaleModeCbx_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PlayerWindow::onScaleModeChanged);
    connect(m_aspectCbx_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PlayerWindow::onAspectChanged);
    connect(m_zoomInBtn_, &QPushButton::clicked, this, [this]() {
        if (m_ffmpegPlayer_)
            m_ffmpegPlayer_->setZoom(m_ffmpegPlayer_->zoom() * 1.25);
    });
    connect(m_zoomOutBtn_, &QPushButton::clicked, this, [this]() {
        if (m_ffmpegPlayer_)
            m_ffmpegPlayer_->setZoom(m_ffmpegPlayer_->zoom() / 1.25);
    });
    connect(m_resetViewBtn_, &QPushButton::clicked, this, &PlayerWindow::resetViewOptions);
    connect(m_lockAspectCbx_, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            applyAspectLock();
    });

    if (m_ffmpegPlayer_)
    {
        connect(m_ffmpegPlayer_, &FFmpegPlayer::zoomChanged, this, &PlayerWindow::onZoomChanged);
    }
    else
    {
        // 其他播放实现没有画面映射控制接口，整行禁用并说明原因
        m_viewRow_->setEnabled(false);
        m_viewRow_->setToolTip(tr("picture scaling is only available with the FFmpeg player"));
    }
}

// ==================== 画面比例与窗口缩放 ====================

void PlayerWindow::onVideoGeometry(int width, int height, int sarNum, int sarDen)
{
    Q_UNUSED(height)

    // 显示比例 = 像素宽高比 * SAR（DVD 标清源必须考虑 SAR，否则画面变形）
    // 优先取播放控件算好的结果：它会把"显示比例覆盖"一并算进去，避免窗口与画面不一致
    m_videoAspect_ = m_ffmpegPlayer_ ? m_ffmpegPlayer_->displayAspect() : 0.0;

    if (m_videoAspect_ <= 0.0)
    {
        if (width > 0 && height > 0 && sarNum > 0 && sarDen > 0)
            m_videoAspect_ = static_cast<double>(width) * sarNum / (static_cast<double>(height) * sarDen);
        else if (width > 0 && height > 0)
            m_videoAspect_ = static_cast<double>(width) / height;
        else
            m_videoAspect_ = 0.0;
    }

    // 首次获取到画面比例时自动适配一次窗口（后续由用户控制，避免打扰）
    if (!m_windowFitted_ && m_videoAspect_ > 0.0)
    {
        m_windowFitted_ = true;
        QTimer::singleShot(0, this, &PlayerWindow::fitWindowToVideo); // 等布局稳定后再调整
    }
}

void PlayerWindow::onScaleModeChanged(int index)
{
    if (!m_ffmpegPlayer_)
        return;

    m_ffmpegPlayer_->setScaleMode(static_cast<VideoScaleMode>(m_scaleModeCbx_->itemData(index).toInt()));
}

void PlayerWindow::onAspectChanged(int index)
{
    if (!m_ffmpegPlayer_)
        return;

    m_ffmpegPlayer_->setAspectOverride(m_aspectCbx_->itemData(index).toDouble());
    // 显示比例覆盖会改变画面的真实显示比例，窗口适配与锁定比例都要跟着换算
    m_videoAspect_ = m_ffmpegPlayer_->displayAspect();
    if (m_lockAspectCbx_->isChecked())
        applyAspectLock();
}

void PlayerWindow::onZoomChanged(double zoom)
{
    m_zoomLabel_->setText(QString::number(qRound(zoom * 100)) + "%");
}

void PlayerWindow::resetViewOptions()
{
    if (m_ffmpegPlayer_)
        m_ffmpegPlayer_->resetView();
    m_zoomLabel_->setText("100%");
}

void PlayerWindow::applyAspectLock()
{
    if (m_applyingAspectLock_ || isFullScreen())
        return;
    if (m_videoAspect_ <= 0.0 || !m_playerWidget_)
        return;

    // 除视频区外窗口其余部分（进度条/控制条/边距）的占位，必须一起算进窗口尺寸，
    // 否则每次调整都会少算边距，窗口越缩越小
    const QSize extra = size() - m_playerWidget_->size();
    const int videoW = qMax(160, width() - extra.width());
    const int videoH = qMax(90, qRound(videoW / m_videoAspect_));

    const int targetH = videoH + extra.height();
    if (targetH == height())
        return;

    m_applyingAspectLock_ = true;
    resize(width(), targetH);
    m_applyingAspectLock_ = false;
}

void PlayerWindow::fitWindowToVideo()
{
    if (m_videoAspect_ <= 0.0 || !m_playerWidget_)
        return;
    if (isFullScreen())
        return;

    const int videoW = m_playerWidget_->width();
    if (videoW <= 0)
        return;

    // 除视频区外窗口其余部分（进度条/控制条/边距）的占位。
    // resize() 作用于整个窗口，必须把它算进去，否则窗口宽度会被设成视频区宽度，
    // 每点一次"fit"窗口都会因为少算边距而缩小一圈。
    const QSize extra = size() - m_playerWidget_->size();

    int targetVideoW = videoW;
    int targetVideoH = qRound(targetVideoW / m_videoAspect_);

    // 限制在屏幕可用区域内（80%），避免超出屏幕
    QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
    const int maxVideoW = qMax(160, avail.width() * 8 / 10 - extra.width());
    const int maxVideoH = qMax(90, avail.height() * 8 / 10 - extra.height());
    if (targetVideoW > maxVideoW || targetVideoH > maxVideoH)
    {
        const double scale = qMin(static_cast<double>(maxVideoW) / targetVideoW,
                                  static_cast<double>(maxVideoH) / targetVideoH);
        targetVideoW = qMax(1, qRound(targetVideoW * scale));
        targetVideoH = qRound(targetVideoW / m_videoAspect_);
    }

    resize(qMax(minimumWidth(), targetVideoW + extra.width()),
           qMax(minimumHeight(), targetVideoH + extra.height()));
}

void PlayerWindow::closeEvent(QCloseEvent *event)
{
    // 子控件不会收到 closeEvent，必须在这里显式释放播放资源：
    // 否则解码线程、媒体文件句柄与音频输出会一直占用
#ifdef CAN_USE_FFMPEG
    if (m_ffmpegPlayer_)
        m_ffmpegPlayer_->shutdown();
#endif
    QWidget::closeEvent(event);
}

void PlayerWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    // 锁定比例：拖动窗口时按视频显示比例同步高度
    if (m_lockAspectCbx_ && m_lockAspectCbx_->isChecked())
        applyAspectLock();
}

void PlayerWindow::toggleFullScreen()
{
    if (isFullScreen())
    {
        showNormal();
        m_urlInput_->setVisible(true);     // 退出全屏恢复 URL 输入行
    }
    else
    {
        m_urlInput_->setVisible(false);    // 全屏时隐藏 URL 输入行，控制条保留
        showFullScreen();
    }
}

void PlayerWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F11)
    {
        toggleFullScreen();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && isFullScreen())
    {
        toggleFullScreen();
        event->accept();
        return;
    }
    // 画面缩放快捷键：+/- 缩放，0 复位，1~4 切换缩放模式（与常见播放器一致）
    if (m_ffmpegPlayer_)
    {
        switch (event->key())
        {
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            m_ffmpegPlayer_->setZoom(m_ffmpegPlayer_->zoom() * 1.25);
            event->accept();
            return;
        case Qt::Key_Minus:
            m_ffmpegPlayer_->setZoom(m_ffmpegPlayer_->zoom() / 1.25);
            event->accept();
            return;
        case Qt::Key_0:
            resetViewOptions();
            event->accept();
            return;
        case Qt::Key_1:
        case Qt::Key_2:
        case Qt::Key_3:
        case Qt::Key_4:
            m_scaleModeCbx_->setCurrentIndex(event->key() - Qt::Key_1);
            event->accept();
            return;
        default:
            break;
        }
    }
    QWidget::keyPressEvent(event);
}

bool PlayerWindow::eventFilter(QObject *watched, QEvent *event)
{
    // 双击视频区域进入/退出全屏
    if (watched == m_playerWidget_ && event->type() == QEvent::MouseButtonDblClick)
    {
        toggleFullScreen();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void PlayerWindow::updateTimeDuration(long long len)
{
    m_duration_ = formatTime(len);
    // 更新视频时长信息
    m_timeInfo_->setText("00:00 / " + m_duration_);
    // 设置进度条范围
    m_progressSlider_->setRange(0, len);
    m_progressSlider_->setEnabled(len > 0); // 启用进度条
}

void PlayerWindow::updateTimePosition(long long pos)
{
    // 更新视频当前播放进度
    m_timeInfo_->setText(formatTime(pos) + " / " + m_duration_);
    // 更新进度条位置
    if (!m_progressSlider_->isSliderDown())
    {
        m_progressSlider_->setValue(pos);
    }
}

void PlayerWindow::updatePlayBtnState(int state)
{
    // 记录最近播放状态（供停止态下的重播判断）
    m_playState_ = static_cast<PlayerWidgetBase::PlayState>(state);
    // 更新播放按钮状态
    m_playBtn_->setIcon(style()->standardIcon(
        state == QMediaPlayer::PlayingState ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

void PlayerWindow::onPlayBtnClicked()
{
    if (m_playState_ == PlayerWidgetBase::PlayingState ||
        m_playState_ == PlayerWidgetBase::PausedState)
    {
        // 播放/暂停切换
        emit m_playerInstance_->changePlayState();
        return;
    }

    // 停止态（或从未播放）：重新播放当前段落；非段落模式则重播上次媒体
    if (m_dvdIndex_ >= 0 && m_dvdIndex_ < m_dvdFiles_.size())
    {
        playSegment(m_dvdIndex_);
    }
    else if (!m_lastMediaSource_.isEmpty())
    {
        if (m_lastMediaIsUrl_)
            emit m_playerInstance_->setPlayerUrl(m_lastMediaSource_);
        else
            emit m_playerInstance_->setPlayerFile(m_lastMediaSource_);
    }
}

void PlayerWindow::updateVolumeSlider(int volume)
{
    // 更新音量滑块位置
    m_volumeSlider_->setValue(volume);
    m_volumeBtn_->setToolTip(tr("volume: %1").arg(volume)); // 设置提示信息
}

void PlayerWindow::updateVolumeBtnState(bool isMute)
{
    // 更新音量按钮状态
    m_volumeBtn_->setIcon(style()->standardIcon(
        isMute ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
}

void PlayerWindow::openFileDialog()
{
    QString filters = tr(
        "Video Files (*.mp4 *.avi *.mkv *.vob *.mpg *.mpeg *.ts *.m2ts *.mov *.wmv *.flv);;"
        "DVD VOB Files (*.vob);;"
        "Audio Files (*.wav *.mp3 *.aac *.flac);;"
        "All Files (*.*)");
    // 打开文件管理器，选择视频文件
    QString startDir = m_lastOpenDir_.isEmpty()
                           ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
                           : m_lastOpenDir_;
    QString filePath = QFileDialog::getOpenFileName(this,
                                                    tr("select media file"),
                                                    startDir,
                                                    filters);

    if (!filePath.isEmpty())
    {
        // 记录目录，便于下次打开定位；退出 DVD 段落模式
        m_lastOpenDir_ = QFileInfo(filePath).absolutePath();
        clearSegmentContext();
        // 记录来源，供停止态重播使用
        m_lastMediaSource_ = filePath;
        m_lastMediaIsUrl_ = false;
        // 调用播放器加载视频文件
        emit m_playerInstance_->setPlayerFile(filePath);
    }
}

void PlayerWindow::triggerLoadUrl()
{
    // 触发接入视频流地址
    QString url = m_urlInput_->text();
    if (!url.isEmpty())
    {
        // 网络流不参与 DVD 段落播放
        clearSegmentContext();
        m_lastMediaSource_ = url;
        m_lastMediaIsUrl_ = true;
        emit m_playerInstance_->setPlayerUrl(url);
    }
}

// ==================== DVD / VOB 目录播放 ====================

void PlayerWindow::openDvdFolder()
{
    QString startDir = m_lastOpenDir_.isEmpty()
                           ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
                           : m_lastOpenDir_;
    QString dir = QFileDialog::getExistingDirectory(this,
                                                    tr("select DVD / VIDEO_TS folder"),
                                                    startDir);
    if (dir.isEmpty())
        return;

    m_lastOpenDir_ = dir;
    loadDvdFolder(dir);
}

void PlayerWindow::loadDvdFolder(const QString &dir)
{
    // 默认跳过 DVD 菜单/引导文件（VIDEO_TS.VOB、VTS_xx_0.VOB），保证正片连续播放；
    // 若因此找不到内容（例如目录里只有菜单文件），再退回加载全部 VOB。
    QStringList vobs = DvdUtils::collectVobSequence(dir, true);
    if (vobs.isEmpty())
        vobs = DvdUtils::collectVobSequence(dir, false);
    if (vobs.isEmpty())
    {
        m_playerInstance_->errorInfoShow(tr("No playable VOB file found in folder:\n%1").arg(dir));
        return;
    }

    m_dvdFiles_ = vobs;
    m_dvdIndex_ = -1;

    // 填充段落下拉框
    m_segmentCbx_->blockSignals(true);
    m_segmentCbx_->clear();
    for (int i = 0; i < m_dvdFiles_.size(); ++i)
    {
        m_segmentCbx_->addItem(
            tr("%1  [%2/%3]").arg(QFileInfo(m_dvdFiles_.at(i)).fileName())
                             .arg(i + 1)
                             .arg(m_dvdFiles_.size()),
            m_dvdFiles_.at(i));
    }
    m_segmentCbx_->setCurrentIndex(-1);
    m_segmentCbx_->blockSignals(false);

    m_segmentRow_->setVisible(true);
    playSegment(0); // 载入后从第一个正片段落开始播放
}

void PlayerWindow::clearSegmentContext()
{
    m_dvdFiles_.clear();
    m_dvdIndex_ = -1;

    if (m_segmentRow_)
    {
        m_segmentRow_->setVisible(false);
        m_segmentCbx_->blockSignals(true);
        m_segmentCbx_->clear();
        m_segmentCbx_->blockSignals(false);
    }
    setWindowTitle(m_baseTitle_);
}

void PlayerWindow::onSegmentActivated(int index)
{
    if (index >= 0)
        playSegment(index);
}

void PlayerWindow::playSegment(int index)
{
    if (index < 0 || index >= m_dvdFiles_.size())
        return;

    m_dvdIndex_ = index;
    // 记录来源，供停止态重播使用
    m_lastMediaSource_ = m_dvdFiles_.at(index);
    m_lastMediaIsUrl_ = false;

    // 同步下拉框显示（避免自动切换时重复触发 activated）
    m_segmentCbx_->blockSignals(true);
    m_segmentCbx_->setCurrentIndex(index);
    m_segmentCbx_->blockSignals(false);

    // 切换段落时重置时间/进度显示（新段打开后会自动刷新）
    m_duration_ = "00:00";
    m_timeInfo_->setText("00:00 / " + m_duration_);
    m_progressSlider_->setRange(0, 0);

    // 窗口标题提示当前段落
    setWindowTitle(tr("%1 - %2 [%3/%4]")
                       .arg(m_baseTitle_)
                       .arg(QFileInfo(m_dvdFiles_.at(index)).fileName())
                       .arg(index + 1)
                       .arg(m_dvdFiles_.size()));

    emit m_playerInstance_->setPlayerFile(m_dvdFiles_.at(index));
}

void PlayerWindow::goPrevSegment()
{
    if (m_dvdFiles_.isEmpty())
        return;
    // 循环切换上一段
    playSegment(m_dvdIndex_ > 0 ? m_dvdIndex_ - 1 : m_dvdFiles_.size() - 1);
}

void PlayerWindow::goNextSegment()
{
    if (m_dvdFiles_.isEmpty())
        return;
    // 循环切换下一段
    playSegment(m_dvdIndex_ < m_dvdFiles_.size() - 1 ? m_dvdIndex_ + 1 : 0);
}

void PlayerWindow::onMediaFinished(const QString &sourceUrl)
{
    // 仅段落播放模式下处理自动连播
    if (m_dvdIndex_ < 0 || m_dvdIndex_ >= m_dvdFiles_.size())
        return;
    // 仅当自然结束的来源确为当前正在播放的段落（避免旧的结束信号干扰新段落）
    if (sourceUrl != m_dvdFiles_.at(m_dvdIndex_))
        return;
    if (!m_autoNextCbx_->isChecked())
        return;
    // 还有后续段落则自动播放下一段；最后一段播完则停留结束画面
    if (m_dvdIndex_ < m_dvdFiles_.size() - 1)
        playSegment(m_dvdIndex_ + 1);
}

#ifdef CAN_USE_FFMPEG
void PlayerWindow::openTranscodeWindow()
{
    // 预填转码输入：整碟段落全部带入（合并转码）；普通文件带当前文件；URL 流不支持转码
    QStringList files;
    if (!m_dvdFiles_.isEmpty())
        files = m_dvdFiles_;
    else if (!m_lastMediaIsUrl_ && !m_lastMediaSource_.isEmpty())
        files << m_lastMediaSource_;

    if (files.isEmpty())
    {
        m_playerInstance_->errorInfoShow(tr("Open a local media file (or DVD folder) first, then convert it."));
        return;
    }

    auto *transcodeWindow = new TranscodeWindow;
    transcodeWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    transcodeWindow->presetInput(files);
    transcodeWindow->show();
    transcodeWindow->raise();
    transcodeWindow->activateWindow();
}
#endif