/*****************************************************************
File:        texture_manager.h
Version:     1.0
Author:
start date:
Description:

Version history
[序号][修改日期][修改者][修改内容]

*****************************************************************/

#ifndef TEXTURE_MANAGER_H_
#define TEXTURE_MANAGER_H_

#include <QHash>
#include <QOpenGLTexture>
#include <QString>

#include "singleton.h"

class TextureManager
{
public:
    // 单例模式，确保全局唯一纹理管理器
    friend SingletonTemplate<TextureManager>;

    // 从文件加载纹理并缓存
    QOpenGLTexture* loadTexture(const QString& path, const QString& textureName);

    // 从 QImage 创建纹理
    QOpenGLTexture* createTexture(const QImage& image, const QString& textureName);

    // 获取缓存的纹理
    QOpenGLTexture* getTexture(const QString& textureName) const;

    // 清理所有纹理
    void cleanup();

private:
    TextureManager() = default;
    ~TextureManager() { cleanup(); }

    QHash<QString, QOpenGLTexture*> m_textures; // 纹理名称到ID的映射
};

#endif // TEXTURE_MANAGER_H_