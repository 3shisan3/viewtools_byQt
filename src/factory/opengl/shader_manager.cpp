#include "shader_manager.h"

#include <QOpenGLShader>
#include <QFile>
#include <regex>
#if QT_VERSION_MAJOR < 6
#include <QRegExp>
using CurRegExp = QRegExp;
#else
#include <QRegularExpression>
using CurRegExp = QRegularExpression;
#endif
#include <QRegularExpression>
#include <QDebug>

QMap<QString, SsSharderManager *> SsSharderManager::m_mapShader;

SsSharderManager::SsSharderManager()
{
    initializeOpenGLFunctions();
}

SsSharderManager::~SsSharderManager()
{
}

// 设置名称
void SsSharderManager::setName(const QString &name)
{
    m_name = name.toStdString();
}

void SsSharderManager::setName(const QString &_vertex, const QString &_fragment)
{
    m_vertex = _vertex.toStdString();
    m_fragment = _fragment.toStdString();
}

// 通过文件中加载shader
void SsSharderManager::loadShaderFromSourceFile(const QString &_vertex, const QString &_fragment)
{
    setName(_vertex, _fragment);

    QFile file_vert(_vertex);

    if (!file_vert.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug() << _vertex << ":文件打开失败";
        return;
    }

    QFile file_frag(_fragment);

    if (!file_frag.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug() << _fragment << ":文件打开失败";
        return;
    }

    loadShaderFromSourceCode(file_vert.readAll(), file_frag.readAll());
}

// 通过文本中加载shader
void SsSharderManager::loadShaderFromSourceCode(const QString &_vertex, const QString &_fragment)
{
    QString _vertex_code = pretreatment(_vertex);
    QString _fragment_code = pretreatment(_fragment);

    if (!addShaderFromSourceCode(QOpenGLShader::Vertex, _vertex_code))
    {

    }

    if (!addShaderFromSourceCode(QOpenGLShader::Fragment, _fragment_code))
    {
        
    }

    // 解析着色器

    // 绑定数据块
}

// 绑定数据块

// 预处理
QString SsSharderManager::pretreatment(const QString &text)
{
    // 移除多行注释
    QString result = text;

    // 移除单行注释
    {
        QRegularExpression regex("//[^\n]*");

        result = result.replace(regex, "");
    }

    // 移除多行注释
    {
        QRegularExpression regex("/\\*.*?\\*/");

        regex.setPatternOptions(QRegularExpression::DotMatchesEverythingOption);

        result = result.replace(regex, "");
    }

    // 移除空白行
    {
        QRegularExpression regex("^[\\s]*$|^\\n$", QRegularExpression::MultilineOption);

        result = result.replace(regex, "");
    }

    return result;
}

// 移除注释
QString SsSharderManager::removeComments(QString &text)
{
    QString result;

    // 移除单行注释
    {
        QRegularExpression regex("//[^\n]*");

        result = text.replace(regex, "");
    }

    // 移除多行注释
    {
        QRegularExpression regex("/\\*.*?\\*/");

        regex.setPatternOptions(QRegularExpression::DotMatchesEverythingOption);

        result = text.replace(regex, "");
    }

    return result;
}

// 从文件中加载着色器
SsSharderManager *SsSharderManager::loadFile(const QString &path)
{
    {
        QFile file(path);

        if (!file.exists())
        {
            qDebug() << "[shader]:" << path << " is not found.";
            return nullptr;
        }
    }

    SsSharderManager *_shader = nullptr;

    if (m_mapShader.contains(path))
    {
        _shader = m_mapShader[path];
    }
    else
    {
        QFile file(path);

        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            qDebug() << path << ":文件打开失败";
            return nullptr;
        }

        QString text = file.readAll();

        int index = text.lastIndexOf("#version 330 core");

        QString vertex = text.left(index);
        QString fragment = text.right(text.length() - index);

        _shader = new SsSharderManager();

        _shader->setName(path);
        _shader->loadShaderFromSourceCode(vertex, fragment);

        m_mapShader.insert(path, _shader);
    }

    return _shader;
}

// 从文件中加载着色器
SsSharderManager *SsSharderManager::loadFile(const QString &_vertex, const QString &_fragment)
{
    SsSharderManager *_shader = nullptr;

    QString _name = _vertex + _fragment;

    if (m_mapShader.contains(_name))
    {
        _shader = m_mapShader[_name];
    }
    else
    {
        _shader = new SsSharderManager();
        _shader->loadShaderFromSourceFile(_vertex, _fragment);
        m_mapShader.insert(_name, _shader);
    }

    return _shader;
}