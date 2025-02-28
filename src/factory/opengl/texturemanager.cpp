#ifdef OPENGL_ENABLE

#include "texturemanager.h"

#include <QDebug>
#include <QVector>

bool TextureManager::initialize()
{
    if (!m_initialized)
    {
        // 初始化 OpenGL 函数
        if (!initializeOpenGLFunctions())
        {
            qCritical() << "Failed to initialize OpenGL functions!";
            return false;
        }
        m_initialized = true;
        qDebug() << "TextureManager initialized with OpenGL context:" << QOpenGLContext::currentContext();
    }
    return true;
}

GLuint TextureManager::loadTexture(const QString &path, const QString &textureName)
{
    if (!m_initialized)
    {
        qWarning() << "TextureManager not initialized!";
        return 0;
    }

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
        return 0;
    }

    return createTexture(image, textureName);
}

GLuint TextureManager::createTexture(const QImage &image, const QString &textureName)
{
    // 转换为 OpenGL 兼容格式
    QImage glImage = image.convertToFormat(QImage::Format_RGBA8888);

    GLuint textureId;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);

    // 设置纹理参数
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 上传纹理数据（使用现代内部格式 GL_RGBA8）
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8,
        glImage.width(), glImage.height(), 0,
        GL_RGBA, GL_UNSIGNED_BYTE, glImage.constBits());

    glBindTexture(GL_TEXTURE_2D, 0);

    // 缓存纹理
    m_textures[textureName] = textureId;
    return textureId;
}

GLuint TextureManager::getTexture(const QString &textureName) const
{
    return m_textures.value(textureName, 0);
}

void TextureManager::cleanup()
{
    if (!m_textures.isEmpty())
    {
        QVector<GLuint> textureIds = m_textures.values().toVector(); // 转换为 QVector
        glDeleteTextures(textureIds.size(), textureIds.data());      // 使用 data() 方法
        m_textures.clear();
        qDebug() << "All textures cleaned up.";
    }
}

#endif