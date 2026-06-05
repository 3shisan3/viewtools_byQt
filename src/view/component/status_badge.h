/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        status_badge.h
Version:     1.0
Author:      cjx
start date:
Description: 通用状态徽标组件
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef STATUS_BADGE_H
#define STATUS_BADGE_H

#include <QLabel>

class SsStatusBadge : public QLabel
{
public:
    enum Status
    {
        Normal,
        Running,
        Warning,
        Error,
        Disabled
    };

    explicit SsStatusBadge(QWidget *parent = nullptr);

    void setStatus(Status status, const QString &text = QString());
    Status status() const { return m_status_; }

private:
    void refreshStyle();

    Status m_status_;
};

#endif  // STATUS_BADGE_H
