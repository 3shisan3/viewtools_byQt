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
#include <QOpenGLTexture>

// 将 QImage 转换为 OpenGL 纹理
static GLuint createTextureFromImage(const QImage &image)
{
    if (image.isNull())
    {
        qWarning() << "Invalid image provided for texture creation.";
        return 0;
    }

    // 未引用系统opengl库场景，需要QOpenGLContext::currentContext()获取且确保上下文有效，加载OpenGL函数指针；或者添加入参外部传入
    // 使用 QOpenGLTexture 创建纹理
    QOpenGLTexture *texture = new QOpenGLTexture(image);
    texture->setMinificationFilter(QOpenGLTexture::Linear);
    texture->setMagnificationFilter(QOpenGLTexture::Linear);

    return texture->textureId();
}
#endif