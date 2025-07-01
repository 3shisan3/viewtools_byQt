/*****************************************************************
File:        qmedia_player_widget.h
Version:     1.0
Author:
start date:
Description: 使用QMediaPlayer(依赖环境底层安装解析器)
    完成的播放功能组件

Version history
[序号][修改日期][修改者][修改内容]

*****************************************************************/

#ifndef _QMEDIA_PLAYER_WIDGET_H
#define _QMEDIA_PLAYER_WIDGET_H

#include <QMediaPlayer>
#include <QVideoWidget>

#include "base_player_widget.h"

// 只做播放需求，使用QVideoWidget，功能丰富可替换
class SsQMediaPlayer : public QVideoWidget
{
    Q_OBJECT
public:
    explicit SsQMediaPlayer(QWidget *parent = nullptr);
    ~SsQMediaPlayer();

    // 获取播放器核心类
    PlayerWidgetBase * playerCore() const; 

protected:
    // 信号转发关联
    void translateSignals();
    // 内部信号信号槽关联
    void setupConnections();

private:
    PlayerWidgetBase * m_playerCore_;       // 主要用于信号统一
    QMediaPlayer * m_mediaPlayer_;          // 播放处理核心
};


#endif  // _QMEDIA_PLAYER_WIDGET_H