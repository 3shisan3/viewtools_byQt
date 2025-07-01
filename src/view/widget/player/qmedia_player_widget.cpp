#include "qmedia_player_widget.h"

#if QT_VERSION_MAJOR < 6
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#else
#include <QMediaDevices>
#include <QAudioOutput>
#include <QAudioSink>
#endif

SsQMediaPlayer::SsQMediaPlayer(QWidget *parent)
    : QVideoWidget(parent)
    , m_playerCore_(new PlayerWidgetBase(this))
    , m_mediaPlayer_(new QMediaPlayer(this))
{
    m_mediaPlayer_->setVideoOutput(this);       // 设置视频输出到当前窗口

    translateSignals();
    setupConnections();
}

SsQMediaPlayer::~SsQMediaPlayer()
{
    // 资源通过Qt父子关系自动释放
}

PlayerWidgetBase * SsQMediaPlayer::playerCore() const
{
    return m_playerCore_;
}

void SsQMediaPlayer::translateSignals()
{
    // 信号转发关联
    connect(m_mediaPlayer_, &QMediaPlayer::durationChanged, m_playerCore_, &PlayerWidgetBase::currentDuration);
    connect(m_mediaPlayer_, &QMediaPlayer::positionChanged, m_playerCore_, &PlayerWidgetBase::currentPosition);
#if QT_VERSION_MAJOR < 6
    connect(m_mediaPlayer_, &QMediaPlayer::stateChanged, this, [this](QMediaPlayer::State state) {
#else
    connect(m_mediaPlayer_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
#endif
        // 播放状态变化
        switch (state) {
        case QMediaPlayer::PlayingState:
            emit m_playerCore_->playStateChanged(PlayerWidgetBase::PlayingState);
            break;
        case QMediaPlayer::PausedState:
            emit m_playerCore_->playStateChanged(PlayerWidgetBase::PausedState);
            break;
        case QMediaPlayer::StoppedState:
            emit m_playerCore_->playStateChanged(PlayerWidgetBase::StoppedState);
            break;
        default:
            break;
        }
    });
#if QT_VERSION_MAJOR < 6
    connect(m_mediaPlayer_, &QMediaPlayer::volumeChanged, m_playerCore_, &PlayerWidgetBase::currentVolume);
    connect(m_mediaPlayer_, &QMediaPlayer::mutedChanged, m_playerCore_, &PlayerWidgetBase::muteStateChanged);
#else
    if (m_mediaPlayer_->audioOutput()) {
        connect(m_mediaPlayer_->audioOutput(), &QAudioOutput::volumeChanged, 
               m_playerCore_, &PlayerWidgetBase::currentVolume);
        connect(m_mediaPlayer_->audioOutput(), &QAudioOutput::mutedChanged, 
               m_playerCore_, &PlayerWidgetBase::muteStateChanged);
    }
#endif
}

void SsQMediaPlayer::setupConnections()
{
    // m_playerCore_ -> m_mediaPlayer_
    connect(m_playerCore_, &PlayerWidgetBase::setPlayerFile, this, [this](const QString &filePath) {
#if QT_VERSION_MAJOR < 6
        m_mediaPlayer_->setMedia(QUrl::fromLocalFile(filePath));
#else
        m_mediaPlayer_->setSource(QUrl::fromLocalFile(filePath));
#endif
        m_mediaPlayer_->play(); // 播放文件
    });
    connect(m_playerCore_, &PlayerWidgetBase::setPlayerUrl, this, [this](const QString &url) {
#if QT_VERSION_MAJOR < 6
        m_mediaPlayer_->setMedia(QUrl(url));
#else
        m_mediaPlayer_->setSource(QUrl(url));
#endif
        m_mediaPlayer_->play(); // 播放网络流地址
    });
    connect(m_playerCore_, &PlayerWidgetBase::changePlayState, this, [this]() {
#if QT_VERSION_MAJOR < 6
        if (m_mediaPlayer_->state() == QMediaPlayer::PlayingState) {
#else
        if (m_mediaPlayer_->playbackState() == QMediaPlayer::PlayingState) {
#endif
            m_mediaPlayer_->pause();
        } else {
            m_mediaPlayer_->play();
        }
    });
    connect(m_playerCore_, &PlayerWidgetBase::seekPlay, this, [this](long long pos) {
        m_mediaPlayer_->setPosition(pos);
    });
    connect(m_playerCore_, &PlayerWidgetBase::changeMuteState, this, [this]() {
#if QT_VERSION_MAJOR < 6
        m_mediaPlayer_->setMuted(!m_mediaPlayer_->isMuted());
#else
        if (m_mediaPlayer_->audioOutput()) {
            m_mediaPlayer_->audioOutput()->setMuted(!m_mediaPlayer_->audioOutput()->isMuted());
        }
#endif
    });
    connect(m_playerCore_, &PlayerWidgetBase::setVolume, this, [this](int volume) {
#if QT_VERSION_MAJOR < 6
        m_mediaPlayer_->setVolume(volume);
#else
        if (m_mediaPlayer_->audioOutput()) {
            m_mediaPlayer_->audioOutput()->setVolume(volume / 100.0f);
        }
#endif
    });
}