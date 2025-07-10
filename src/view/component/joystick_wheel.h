/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        joystick_wheel.h
Version:     1.0
Author:      cjx
start date: 
Description: 模拟手柄摇杆的轮盘组件

Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef JOYSTICK_WHEEL_H
#define JOYSTICK_WHEEL_H

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTouchEvent>
#include <QWidget>

class SsJoystickWheel : public QWidget
{
    Q_OBJECT
public:
    SsJoystickWheel(QWidget *parent = nullptr);
    ~SsJoystickWheel();

    // 设置绘制纹理
    void setBottomTexture(const QString &path);
    void setTopTexture(const QString &path);

    // 设置显示尺寸
    void setOuterCirRadius(uint radius);
    void setInnerCirRadius(uint radius);

    // 获取轮盘一些信息
    uint radius() const;        // 当前轮盘半径

signals:
    void curRockerBarPos(QPoint pos, QPoint center);

protected:
    bool event(QEvent *event) override;

    // 绘制事件
    void paintEvent(QPaintEvent *event) override;

    // 尺寸变更
    void resizeEvent(QResizeEvent *event) override;

    // 鼠标事件
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void updatePosition();
    void handleTouchPoint(const QPointF &pos, bool isRelease = false);

private:
    QPoint m_bgWheel_xy;          // 背景轮盘坐标
    QPoint m_rockerBar_xy;        // 摇杆所处坐标
    std::pair<QPixmap, QPixmap> m_texture;        // 纹理组合 first 为底图
    std::pair<uint, uint> m_cirRadius;            // 尺寸半径组合

    bool m_hasPressed;            // 控制操作状态
    int m_activeTouchId;          // 当前激活的触控点ID (-1表示无)(解决多点触控干扰问题)

};

#endif  // JOYSTICK_WHEEL_H