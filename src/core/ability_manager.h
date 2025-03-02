/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:
Version:     1.0
Author:      cjx
start date:
Description: 一些常用的功能
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]


*****************************************************************/

#pragma once

#include <QDebug>
#include <QImage>

#ifdef SVG_ENABLE
#include <QPainter>
#include <QSvgRenderer>

// 提供转为QImage的方法，该形式，用于图像处理；转为opengl纹理；临时缓存等方便使用
// 简单地显示使用 QSvgWidget 或 QSvgRenderer 直接渲染
static QImage loadSvgToImage(const QString &svgPath, const QSize &size)
{
    QSvgRenderer renderer(svgPath);
    if (!renderer.isValid())
    {
        qWarning() << "Failed to load SVG file:" << svgPath;
        return QImage();
    }

    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    renderer.render(&painter);
    return image;
}
#endif

#ifdef OPENGL_ENABLE
#include "factory/opengl/texture_manager.h"
#endif