#include "player_window.h"

#include <QFileDialog>
#include <QStandardPaths>
#include <QString>
#include <QStyle>
#include <QVBoxLayout>

#include "utils/option.h"
#include "view/widget/player/base_player_widget.h"
#include "view/widget/player/ffmpeg_player_widget.h"
#include "view/widget/player/qmedia_player_widget.h"

static QString formatTime(qint64 milliseconds)
{
    QTime time((milliseconds / 3600000) % 24,
               (milliseconds / 60000) % 60,
               (milliseconds / 1000) % 60);
    return milliseconds >= 3600000 ? time.toString("hh:mm:ss") : time.toString("mm:ss");
}

PlayerWindow::PlayerWindow(QWidget *parent, PlayerWidget type)
    : QWidget(parent), m_loadFileBtn_(new QPushButton(tr("open file"))), m_urlInput_(new QLineEdit), m_loadUrlBtn_(new QPushButton(tr("open url"))), m_playBtn_(new QPushButton), m_progressSlider_(new QSlider(Qt::Horizontal)), m_volumeBtn_(new QPushButton), m_volumeSlider_(new QSlider(Qt::Horizontal)), m_timeInfo_(new QLabel("00:00 / 00:00")), m_duration_("00:00")
{
    // 通过type m_playerWidget_指向不同的子类widget
    switch (type)
    {
    case PlayerWidget::BY_QMEDIA:
    {
        m_playerWidget_ = new SsQMediaPlayer(this);
        m_playerInstance_ = static_cast<SsQMediaPlayer *>(m_playerWidget_)->playerCore(); // 获取核心类
        // 设置窗口标题
        setWindowTitle(tr("Video Player By QMediaPlayer"));
    }
        break;
    case PlayerWidget::BY_FFMPEG:
    {
        m_playerWidget_ = new FFmpegPlayer(this);
        m_playerInstance_ = static_cast<FFmpegPlayer *>(m_playerWidget_)->playerCore(); // 获取核心类
        setWindowTitle(tr("Video Player By FFmpeg"));
    }
        break;
    default:
        break;
    }

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
    m_playBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_volumeBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));

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
    mainLayout->addWidget(m_playerWidget_);
    mainLayout->addWidget(m_progressSlider_);

    QHBoxLayout *abilityLayout = new QHBoxLayout;

    QHBoxLayout *controlLayout = new QHBoxLayout;
    controlLayout->addWidget(m_loadFileBtn_);
    controlLayout->addWidget(m_loadUrlBtn_);
    controlLayout->addWidget(m_playBtn_);
    controlLayout->addStretch();

    QHBoxLayout *volumeLayout = new QHBoxLayout;
    volumeLayout->addWidget(m_volumeBtn_);
    volumeLayout->addWidget(new QLabel(tr("volume")));
    volumeLayout->addWidget(m_volumeSlider_);

    abilityLayout->addLayout(controlLayout);
    abilityLayout->addLayout(volumeLayout);
    abilityLayout->addWidget(m_timeInfo_);
    abilityLayout->addStretch();

    mainLayout->addWidget(m_urlInput_);
    mainLayout->addLayout(abilityLayout);
}

void PlayerWindow::setupConnections()
{
    // 连接内部信号槽
    connect(m_loadFileBtn_, &QPushButton::clicked, this, &PlayerWindow::openFileDialog);
    connect(m_loadUrlBtn_, &QPushButton::clicked, this, &PlayerWindow::triggerLoadUrl);

    // 转发播放器控件的信号
    connect(m_progressSlider_, &QSlider::sliderMoved, m_playerInstance_, &PlayerWidgetBase::seekPlay);
    connect(m_volumeSlider_, &QSlider::sliderMoved, m_playerInstance_, &PlayerWidgetBase::setVolume);
    connect(m_playBtn_, &QPushButton::clicked, m_playerInstance_, &PlayerWidgetBase::changePlayState);
    connect(m_volumeBtn_, &QPushButton::clicked, m_playerInstance_, &PlayerWidgetBase::changeMuteState);

    // 连接播放器的信号到槽函数
    connect(m_playerInstance_, &PlayerWidgetBase::currentDuration, this, &PlayerWindow::updateTimeDuration);
    connect(m_playerInstance_, &PlayerWidgetBase::currentPosition, this, &PlayerWindow::updateTimePosition);
    connect(m_playerInstance_, &PlayerWidgetBase::playStateChanged, this, &PlayerWindow::updatePlayBtnState);
    connect(m_playerInstance_, &PlayerWidgetBase::currentVolume, this, &PlayerWindow::updateVolumeSlider);
    connect(m_playerInstance_, &PlayerWidgetBase::muteStateChanged, this, &PlayerWindow::updateVolumeBtnState);
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
    // 更新播放按钮状态
    m_playBtn_->setIcon(style()->standardIcon(
        state == QMediaPlayer::PlayingState ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
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
        "Video Files (*.mp4 *.avi *.mkv);;"
        "Audio Files (*.wav *.mp3);;"
        "All Files (*.*)");
    // 打开文件管理器，选择视频文件
    QString filePath = QFileDialog::getOpenFileName(this,
                                                    tr("select media file"),
                                                    QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
                                                    filters);

    if (!filePath.isEmpty())
    {
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
        emit m_playerInstance_->setPlayerUrl(url);
    }
}