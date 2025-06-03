/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        baseGLScene.h
Version:     1.0
Author:      cjx
start date: 
Description: 
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef BASE_OPENGL_SCENE_H_
#define BASE_OPENGL_SCENE_H_

#include <QOpenGLWidget>
#include <QOpenGLFunctions>

class SsBaseGLScene : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit SsBaseGLScene(QWidget *parent = nullptr);
    virtual ~SsBaseGLScene();



protected:
    // 初始化Opengl窗口
    virtual void initializeGL() override;

    // 窗口大小改变时设置视窗大小
    virtual void resizeGL(int w, int h) override;

    // 渲染主体
    virtual void paintGL() override;


private:
    QColor m_backgroundColor_;   // 背景颜色

};

#endif  // BASE_OPENGL_SCENE_H_