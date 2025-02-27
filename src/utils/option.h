/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        option.h
Version:     1.0
Author:      cjx
start date: 
Description: qt内常见操作封装
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#pragma once

#include <QDebug>
#include <QFile>
#include <QWidget>

static void loadStyleSheetFromResource(const QString &path, QWidget *widget)
{
    QFile styleFile(path);
    if (styleFile.open(QFile::ReadOnly))
    {
        QString styleSheet = QLatin1String(styleFile.readAll());
        widget->setStyleSheet(styleSheet); // 应用样式表
        styleFile.close();
    }
    else
    {
        qWarning() << "Failed to open style sheet resource:" << path;
    }
}