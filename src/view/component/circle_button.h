/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        circle_button.h
Version:     1.0
Author:      cjx
start date: 
Description: 自定义的圆形按钮

Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef CIRCLE_BUTTON_H
#define CIRCLE_BUTTON_H

#include <QPushButton>

class SsCircleButton : public QPushButton
{
    Q_OBJECT
public:
    explicit SsCircleButton(QWidget *parent = nullptr);
    explicit SsCircleButton(const QString &text, QWidget *parent = nullptr);
    
protected:
    // 形状控制
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent *event) override;
    
    // 确保圆形区域响应点击
    bool hitButton(const QPoint &pos) const override;
    
private:
    void init();
};

#endif // CIRCLE_BUTTON_H