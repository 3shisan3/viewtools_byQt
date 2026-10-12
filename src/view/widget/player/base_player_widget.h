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

/**
 * @brief 画面缩放模式
 *
 * 参考 mpv/VLC 的 video-fit 概念，统一各播放器实现的画面适配行为：
 * 播放窗口通过该枚举控制画面在窗口内的映射方式，具体几何计算由播放控件完成。
 */
enum VideoScaleMode
{
    FitWindow = 0,      ///< 适应窗口：完整显示画面并保持比例，多余区域留黑边（默认）
    FillWindow,         ///< 铺满窗口：保持比例放大到铺满窗口，超出部分被裁掉，不留黑边
    StretchToWindow,    ///< 拉伸铺满：不保持比例，强行填满整个窗口
    OriginalSize        ///< 原始尺寸：按 1:1 像素显示（可与缩放倍率叠加）
};

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
    void currentPlaybackRate(float rate);           // 当前播放倍速
    // 视频画面几何信息（像素尺寸 + 像素宽高比，用于按显示比例渲染）
    void videoGeometryChanged(int width, int height, int sarNum, int sarDen);

    // 显示窗口 -> 播放控件
    void setPlayerFile(const QString &filePath);    // 指定播放文件
    void setPlayerUrl(const QString &url);          // 播放网络流地址
    // 播放控制
    void changePlayState();                         // 改变一次当前播放状态
    void seekPlay(long long pos);                   // 跳转播放位置
    void setPlaybackRate(float rate);               // 控制视频倍速播放
    // 音量控制
    void changeMuteState();                         // 改变一次当前音量状态
    void setVolume(int volume);                     // 设置音量
    // 播放结束（媒体自然播放到文件末尾触发，参数为结束时播放的来源路径/URL）
    void mediaFinished(const QString &sourceUrl);   // 来源地址（本地文件路径或流地址）

public slots:
    // 错误信息整合弹窗提示
    void errorInfoShow(const QString &errorInfo);

};


#endif  // _BASE_PLAYER_WIDGET_H