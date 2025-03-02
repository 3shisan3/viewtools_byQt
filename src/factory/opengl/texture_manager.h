/*****************************************************************
File:        texture_manager.h
Version:     1.0
Author:
start date:
Description:

Version history
[序号][修改日期][修改者][修改内容]

*****************************************************************/

#ifndef _TEXTURE_MANAGER_H_
#define _TEXTURE_MANAGER_H_

#include <QOpenGLFunctions_3_3_Core>
#include <QImage>
#include <QHash>
#include <QString>

#include "singleton.h"

class TextureManager : protected QOpenGLFunctions_3_3_Core
{
public:
    // 单例模式，确保全局唯一纹理管理器
    friend SingletonTemplate<TextureManager>;

    // 初始化函数（必须在 OpenGL 上下文就绪后调用）
    bool initialize();

    // 从文件加载纹理并缓存
    GLuint loadTexture(const QString &path, const QString &textureName);

    // 从 QImage 创建纹理
    GLuint createTexture(const QImage &image, const QString &textureName);

    // 获取缓存的纹理
    GLuint getTexture(const QString &textureName) const;

    // 清理所有纹理
    void cleanup();

private:
    TextureManager() = default;
    ~TextureManager() { cleanup(); }

    QHash<QString, GLuint> m_textures; // 纹理名称到ID的映射
    bool m_initialized = false;        // 标记是否已初始化
};

#endif //_TEXTURE_MANAGER_H_