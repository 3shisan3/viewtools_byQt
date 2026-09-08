#include "player_window.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStyle>
#include <QTime>
#include <QVBoxLayout>

#include <algorithm>

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

namespace
{
struct DvdVobItem
{
    QString path;      // 绝对路径
    QString name;      // 文件名
    int title = 0;     // 标题号（VTS_xx）
    int seg = 0;       // 段落号（VTS_xx_y 中的 y）
    bool isMenu = false; // 是否 DVD 菜单/引导文件
};

// 收集文件夹内的所有 VOB，按 DVD 命名规则（VTS_xx_y.VOB）排序成自然播放顺序。
// skipMenus 为 true 时跳过菜单/引导类文件（VIDEO_TS.VOB 与 VTS_xx_0.VOB）。
QStringList collectDvdVobs(const QString &dirPath, bool skipMenus)
{
    QDir dir(dirPath);
    if (!dir.exists())
        return QStringList();

    const QRegularExpression vtsRe("^VTS_(\\d+)_(\\d+)\\.vob$",
                                   QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression vmRe("^VIDEO_TS\\.vob$",
                                  QRegularExpression::CaseInsensitiveOption);

    QVector<DvdVobItem> items;
    const QFileInfoList infos = dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &fi : infos)
    {
        if (fi.suffix().compare("vob", Qt::CaseInsensitive) != 0)
            continue;

        DvdVobItem item;
        item.path = fi.absoluteFilePath();
        item.name = fi.fileName();

        const QRegularExpressionMatch vtsMatch = vtsRe.match(item.name);
        if (vtsMatch.hasMatch())
        {
            item.title = vtsMatch.captured(1).toInt();
            item.seg = vtsMatch.captured(2).toInt();
            item.isMenu = (item.seg == 0);
        }
        else if (vmRe.match(item.name).hasMatch())
        {
            // VIDEO_TS.VOB：主菜单/引导，归到最前
            item.title = -1;
            item.seg = -1;
            item.isMenu = true;
        }
        else
        {
            // 非标准命名的 VOB：放到标准正片之后（按文件名排序）
            item.title = 100000;
            item.seg = 100000;
        }
        items.append(item);
    }

    if (items.isEmpty())
        return QStringList();

    std::sort(items.begin(), items.end(), [](const DvdVobItem &a, const DvdVobItem &b) {
        if (a.title != b.title)
            return a.title < b.title;
        if (a.seg != b.seg)
            return a.seg < b.seg;
        return a.name < b.name;
    });

    // 若文件夹里全是菜单文件则原样保留，避免出现"无内容"
    int menuCount = 0;
    for (const DvdVobItem &it : items)
        if (it.isMenu)
            menuCount++;

    QStringList result;
    for (const DvdVobItem &it : items)
        if (!(skipMenus && it.isMenu && menuCount < items.size()))
            result << it.path;
    return result;
}
} // namespace

PlayerWindow::PlayerWindow(QWidget *parent, PlayerWidget type)
    : QWidget(parent), m_loadFileBtn_(new QPushButton(tr("open file"))), m_openDvdBtn_(new QPushButton(tr("open dvd folder"))), m_urlInput_(new QLineEdit), m_loadUrlBtn_(new QPushButton(tr("open url"))), m_playBtn_(new QPushButton), m_progressSlider_(new QSlider(Qt::Horizontal)), m_volumeBtn_(new QPushButton), m_volumeSlider_(new QSlider(Qt::Horizontal)), m_timeInfo_(new QLabel("00:00 / 00:00")), m_segmentRow_(new QWidget(this)), m_prevSegBtn_(new QPushButton(tr("prev"))), m_segmentCbx_(new QComboBox), m_nextSegBtn_(new QPushButton(tr("next"))), m_autoNextCbx_(new QCheckBox(tr("auto next segment"))), m_duration_("00:00"), m_dvdIndex_(-1), m_playState_(PlayerWidgetBase::StoppedState), m_lastMediaIsUrl_(false)
{
    // 通过type m_playerWidget_指向不同的子类widget
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
        m_playerWidget_ = new FFmpegPlayer(this);
        m_playerInstance_ = static_cast<FFmpegPlayer *>(m_playerWidget_)->PlayerCore(); // 获取核心类
        setWindowTitle(tr("Video Player By FFmpeg"));
    }
        break;
#endif
    default:
        break;
    }

    // 记录基础窗口标题（切换段落时在其上拼接段落信息）
    m_baseTitle_ = windowTitle();

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
    m_playBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_volumeBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));

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
    controlLayout->addWidget(m_openDvdBtn_);
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

    // DVD 段落导航行：默认隐藏，载入 DVD 目录后显示
    QHBoxLayout *segmentLayout = new QHBoxLayout(m_segmentRow_);
    segmentLayout->setContentsMargins(0, 0, 0, 0);
    segmentLayout->addWidget(m_prevSegBtn_);
    segmentLayout->addWidget(m_segmentCbx_, 1);
    segmentLayout->addWidget(m_nextSegBtn_);
    segmentLayout->addWidget(m_autoNextCbx_);
    m_segmentRow_->setVisible(false);

    mainLayout->addWidget(m_urlInput_);
    mainLayout->addLayout(abilityLayout);
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
    connect(m_segmentCbx_, QOverload<int>::of(&QComboBox::activated),
            this, &PlayerWindow::onSegmentActivated);

    // 转发播放器控件的信号
    connect(m_progressSlider_, &QSlider::sliderMoved, m_playerInstance_, &PlayerWidgetBase::seekPlay);
    connect(m_volumeSlider_, &QSlider::sliderMoved, m_playerInstance_, &PlayerWidgetBase::setVolume);
    // 播放按钮由窗口统一处理：停止态点击时重新播放（当前段落或上次媒体）
    connect(m_playBtn_, &QPushButton::clicked, this, &PlayerWindow::onPlayBtnClicked);
    connect(m_volumeBtn_, &QPushButton::clicked, m_playerInstance_, &PlayerWidgetBase::changeMuteState);

    // 连接播放器的信号到槽函数
    connect(m_playerInstance_, &PlayerWidgetBase::currentDuration, this, &PlayerWindow::updateTimeDuration);
    connect(m_playerInstance_, &PlayerWidgetBase::currentPosition, this, &PlayerWindow::updateTimePosition);
    connect(m_playerInstance_, &PlayerWidgetBase::playStateChanged, this, &PlayerWindow::updatePlayBtnState);
    connect(m_playerInstance_, &PlayerWidgetBase::currentVolume, this, &PlayerWindow::updateVolumeSlider);
    connect(m_playerInstance_, &PlayerWidgetBase::muteStateChanged, this, &PlayerWindow::updateVolumeBtnState);

    // 自然播放结束（用于 DVD 段落自动连播）
    connect(m_playerInstance_, &PlayerWidgetBase::mediaFinished, this, &PlayerWindow::onMediaFinished);
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
    QStringList vobs = collectDvdVobs(dir, true);
    if (vobs.isEmpty())
        vobs = collectDvdVobs(dir, false);
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