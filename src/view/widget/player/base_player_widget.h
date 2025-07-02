/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        base_player_widget.h
Version:     1.0
Author:      cjx
start date: 
Description: 考虑qt元组多继承问题，此类主要作为同功能类的信号统一
    使用enable_shared_from_this,还需要禁用Qt对象树机制，复杂化，暂时弃用
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef _BASE_PLAYER_WIDGET_H
#define _BASE_PLAYER_WIDGET_H

#include <QWidget>

class PlayerWidgetBase : public QObject
{
    Q_OBJECT
public:
    explicit PlayerWidgetBase(QObject *parent = nullptr);
    ~PlayerWidgetBase();

    enum PlayState
    {
        StoppedState,
        PlayingState,
        PausedState
    };

signals:
    // 播放控件 -> 显示窗口
    void currentDuration(long long len);            // 当前视频总时长
    void currentPosition(long long pos);            // 当前播放进度
    void playStateChanged(PlayState state);         // 播放状态变化
    void currentVolume(int volume);                 // 当前音量
    void muteStateChanged(bool isMute);             // 静音状态变化

    // 显示窗口 -> 播放控件
    void setPlayerFile(const QString &filePath);    // 指定播放文件
    void setPlayerUrl(const QString &url);          // 播放网络流地址
    // 播放控制
    void changePlayState();                         // 改变一次当前播放状态
    void seekPlay(long long pos);                   // 跳转播放位置
    // 音量控制
    void changeMuteState();                         // 改变一次当前音量状态
    void setVolume(int volume);                     // 设置音量

public slots:
    // 错误信息整合弹窗提示
    void errorInfoShow(const QString &errorInfo);

};


#endif  // _BASE_PLAYER_WIDGET_H