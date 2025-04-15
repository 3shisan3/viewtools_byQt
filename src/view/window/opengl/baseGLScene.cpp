#include "baseGLScene.h"

SsBaseGLScene::SsBaseGLScene(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);                                // 必须设置才能接收键盘事件
    setMouseTracking(true);                                         // 必须设置才能接收无按钮按下的鼠标移动事件

    // 在构造函数中设置 OpenGL 上下文格式
    QSurfaceFormat format;
    format.setVersion(4, 5);                                        // OpenGL 4.5
    format.setProfile(QSurfaceFormat::CoreProfile);                 // 兼容模式
    format.setSamples(4);                                           // 启用4x MSAA
    setFormat(format);                                              // 关键：为当前 widget 设置格式

}

SsBaseGLScene::~SsBaseGLScene()
{

}

void SsBaseGLScene::initializeGL()
{
    initializeOpenGLFunctions();

    glClearColor(m_backgroundColor_.redF(), m_backgroundColor_.greenF(), m_backgroundColor_.blueF(), 1.0f);

    
}

void SsBaseGLScene::resizeGL(int w, int h)
{

}

void SsBaseGLScene::paintGL()
{
    
}