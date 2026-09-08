/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        player_window.h
Version:     1.0
Author:      cjx
start date: 
Description: 播放窗口界面
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef _PLAYER_WINDOW_H
#define _PLAYER_WINDOW_H

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QStringList>
#include <QWidget>

#include "view/widget/player/base_player_widget.h"

enum PlayerWidget
{
    BY_QMEDIA,
    BY_FFMPEG
};

class PlayerWindow : public QWidget
{
    Q_OBJECT
public:
    explicit PlayerWindow(QWidget *parent = nullptr, PlayerWidget type = PlayerWidget::BY_QMEDIA);
    ~PlayerWindow();

protected:
    void initWidgetInfo();
    void initLayout();
    void setupConnections();

private slots:
    // 更新界面视频时长信息及控件状态
    void updateTimeDuration(long long len);
    void updateTimePosition(long long pos);
    // 更新界面控件状态
    void updatePlayBtnState(int state);
    void updateVolumeSlider(int volume);
    void updateVolumeBtnState(bool isMute);

    // 打开文件管理器，选择视频文件
    void openFileDialog();
    // 触发接入视频流地址
    void triggerLoadUrl();

    // 播放/暂停按钮：处于停止态时点击将重新播放当前段落或上次媒体
    void onPlayBtnClicked();

    // DVD / VOB 段落播放
    void openDvdFolder();                       // 打开 DVD 目录（VIDEO_TS）
    void onSegmentActivated(int index);         // 用户在下拉框选择段落
    void playSegment(int index);                // 播放指定段落
    void goPrevSegment();                       // 上一段
    void goNextSegment();                       // 下一段
    void onMediaFinished(const QString &sourceUrl);   // 当前段落自然播放结束

private:
    void loadDvdFolder(const QString &dir);     // 加载并解析 DVD 目录内的 VOB 段落
    void clearSegmentContext();                 // 清除段落播放上下文（单文件/URL模式）

    QWidget * m_playerWidget_;                  // 核心播放组件的父类
    PlayerWidgetBase * m_playerInstance_;       // 父类信息实际获取核心
    // 播放内容接入
    QPushButton * m_loadFileBtn_;               // 打开文件管理器导入
    QPushButton * m_openDvdBtn_;                // 打开 DVD 目录
    QLineEdit * m_urlInput_;                    // 输入 RTSP 链接
    QPushButton * m_loadUrlBtn_;                // 接入 RTSP 视频流
    // 播放进程控制
    QPushButton * m_playBtn_;                   // 播放/暂停按钮
    QSlider * m_progressSlider_;                // 视频进度条
    QPushButton * m_volumeBtn_;                 // 声音控制按钮
    QSlider * m_volumeSlider_;                  // 音量大小指示
    // 信息显示
    QLabel * m_timeInfo_;                       // 视频当前时间/总时长
    // DVD 段落导航
    QWidget * m_segmentRow_;                    // 段落导航整行（载入DVD后显示）
    QPushButton * m_prevSegBtn_;                // 上一段
    QComboBox * m_segmentCbx_;                  // 段落下拉选择
    QPushButton * m_nextSegBtn_;                // 下一段
    QCheckBox * m_autoNextCbx_;                 // 自动连播下一段

    QString m_duration_;                        // 当前视频总时长
    QString m_baseTitle_;                       // 窗口基础标题（构造时设置）
    QStringList m_dvdFiles_;                    // 当前 DVD 文件夹内待播的 VOB 列表（顺序即播放顺序）
    int m_dvdIndex_;                            // 当前播放段下标（-1 表示非段落模式）
    QString m_lastOpenDir_;                     // 上次打开的目录（便于再次定位）
    PlayerWidgetBase::PlayState m_playState_;   // 最近一次播放状态
    QString m_lastMediaSource_;                 // 最近一次播放的来源（文件路径或URL，停止态重播用）
    bool m_lastMediaIsUrl_;                     // 最近来源是否为网络流地址
};


#endif  // _PLAYER_WINDOW_H