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

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QWidget>

// 前置声明
class PlayerWidgetBase;

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
    

private:
    QWidget * m_playerWidget_;                  // 核心播放组件的父类
    PlayerWidgetBase * m_playerInstance_;       // 父类信息实际获取核心
    // 播放内容接入
    QPushButton * m_loadFileBtn_;               // 打开文件管理器导入
    QLineEdit * m_urlInput_;                    // 输入 RTSP 链接
    QPushButton * m_loadUrlBtn_;                // 接入 RTSP 视频流
    // 播放进程控制
    QPushButton * m_playBtn_;                   // 播放/暂停按钮
    QSlider * m_progressSlider_;                // 视频进度条
    QPushButton * m_volumeBtn_;                 // 声音控制按钮
    QSlider * m_volumeSlider_;                  // 音量大小指示
    // 信息显示
    QLabel * m_timeInfo_;                       // 视频当前时间/总时长

    QString m_duration_;                        // 当前视频总时长
};


#endif  // _PLAYER_WINDOW_H