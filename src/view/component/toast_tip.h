/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        toast_tip.h
Version:     1.0
Author:      cjx
start date:
Description: 通用轻量提示组件
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef TOAST_TIP_H
#define TOAST_TIP_H

#include <QFrame>

class QLabel;
class QTimer;

class SsToastTip : public QFrame
{
public:
    enum TipType
    {
        Info,
        Success,
        Warning,
        Error
    };

    explicit SsToastTip(QWidget *parent = nullptr);

    void showMessage(const QString &message, TipType type = Info, int durationMs = 2000);

    static void showTip(QWidget *parent, const QString &message, TipType type = Info, int durationMs = 2000);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateStyle(TipType type);
    void updatePosition();

    QLabel *m_label_;
    QTimer *m_hideTimer_;
};

#endif  // TOAST_TIP_H
