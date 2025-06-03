/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        shader_manager.h
Version:     1.0
Author:      cjx
start date: 
Description: 着色器加载管理
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef SHADER_MANAGER_H_
#define SHADER_MANAGER_H_

#include <QOpenGLFunctions>
#include <QOpenGLShader>
#include <QOpenGLShaderProgram>

class SsSharderManager : public QOpenGLShaderProgram, QOpenGLFunctions
{
public:
    SsSharderManager();
    ~SsSharderManager();

public:
    // 设置名称
    void setName(const QString &name);

    void setName(const QString &_vertex, const QString &_fragment);

    // 通过文件中加载shader
    void loadShaderFromSourceFile(const QString &_vertex, const QString &_fragment);

    // 通过文本中加载shader
    void loadShaderFromSourceCode(const QString &_vertex, const QString &_fragment);

    // 将缓冲对象指定到对于的绑定点

    // 绑定纹理

protected:
    // 预处理
    QString pretreatment(const QString &text);

    // 移除注释
    QString removeComments(QString &text);

    // 绑定Uniform块（频繁设置shader的参数会影响渲染速度，因此采用UBO机制来处理全局属性）

    std::string m_name;     ///< 顶点着色器&片段着色器的路径
    std::string m_vertex;   ///< 顶点着色器的路径
    std::string m_fragment; ///< 片段着色器的路径


public:
    // 从文件中加载着色器
    static SsSharderManager *loadFile(const QString &path);

    static SsSharderManager *loadFile(const QString &_vertex, const QString &_fragment);

protected:
    static QMap<QString, SsSharderManager *> m_mapShader;
};




#endif // SHADER_MANAGER_H_