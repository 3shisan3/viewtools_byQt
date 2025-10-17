#ifdef OPENGL_ENABLE

#include "texture_manager.h"

#include <QDebug>

QOpenGLTexture *TextureManager::loadTexture(const QString &path, const QString &textureName)
{
    // 如果纹理已缓存，直接返回
    if (m_textures.contains(textureName))
    {
        return m_textures.value(textureName);
    }

    // 加载图像
    QImage image(path);
    if (image.isNull())
    {
        qWarning() << "Failed to load image:" << path;
        return nullptr;
    }

    return createTexture(image, textureName);
}

QOpenGLTexture *TextureManager::createTexture(const QImage &image, const QString &textureName)
{
    // 创建 QOpenGLTexture 对象（自动处理 OpenGL 初始化）
#if QT_VERSION_MAJOR < 6
    QOpenGLTexture *texture = new QOpenGLTexture(image.mirrored());
#else
    QOpenGLTexture *texture = new QOpenGLTexture(image.flipped(Qt::Vertical));
#endif
    texture->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear, QOpenGLTexture::Linear);
    texture->setWrapMode(QOpenGLTexture::Repeat);
    texture->generateMipMaps();

    // 缓存纹理
    m_textures[textureName] = texture;

    return texture;
}

QOpenGLTexture *TextureManager::getTexture(const QString &textureName) const
{
    return m_textures.value(textureName, nullptr);
}

void TextureManager::cleanup()
{
    if (!m_textures.isEmpty())
    {
        qDeleteAll(m_textures); // 自动释放 QOpenGLTexture 对象
        m_textures.clear();
        qDebug() << "All textures cleaned up.";
    }
}

#endif