/*****************************************************************
File:        fixedPipelineGL.h
Version:     1.0
Author:
start date:
Description:
    基于固定管道渲染的绘制窗口及方法类
    方便快速实现，效果检验逐渐废弃    
Version history
[序号][修改日期][修改者][修改内容]

*****************************************************************/

#ifndef QT_OPENGL_ES_2      // 安卓不支持固定渲染管线

#ifndef FIXED_PIPELINE_OPENGL_VIEW_H_
#define FIXED_PIPELINE_OPENGL_VIEW_H_

#include <QOpenGLFunctions_2_1>
#include <QOpenGLTexture>
#include <QOpenGLWidget>
#include <QVector2D>
#include <QVector3D>


class SsFixedPipelineGLWidgetBase : public QOpenGLWidget, QOpenGLFunctions_2_1 
{
    Q_OBJECT
public:
    explicit SsFixedPipelineGLWidgetBase(QWidget * parent = nullptr);
    virtual ~SsFixedPipelineGLWidgetBase();

    // 坐标转换(默认QVector3D z = center.z)
    QPointF ndcTrans(const QVector3D &point);
    QVector3D ndcTrans(const QPointF &point);

    // 设置背景颜色
    void setBackgroundColor(const QColor &color);

    // 设置视图坐标系中心点(子类也可通过此方法在构造是初始化中心坐标系)
    void setViewCenter(const QVector3D &pos);
    void setCoordFactor(const float value);
    // 功能同上
    void setViewRect(const QVector3D &topLeft, const QVector3D &bottomRight);

    // 当前是否在拾取状态（外部查询，便于给绘制要素glPushName)
    bool isPickMode() const;


    /* 基础绘制接口封装，入参的点坐标系，依赖于上下文中，当前处理的什么矩阵(默认模型视图矩阵--模型坐标系=世界坐标系) */

    // 绘制点
    void drawPoint(const QVector3D &point, float size = 1.0f, const QColor &color = Qt::yellow);

    // 绘制线
    void drawLine(const QVector3D &start, const QVector3D &end, float width = 1.0f, const QColor &color = Qt::white);

    // 绘制多边形
    void drawPolygon(const QVector<QVector3D> &points, const QColor &borderColor = Qt::transparent, const QColor &fillColor = Qt::transparent);

    // 绘制文字(使用视图逻辑坐标)
    void drawText(const QPointF &position, const QString &text, const QColor &color, int fontSize = 13);

    // 绘制纹理(3d纹理需额外生成)
    void drawTexture(const QVector2D &position, const QSize &size, QOpenGLTexture *texture);

protected:
    // 初始化Opengl窗口
    virtual void initializeGL() override;

    // 窗口大小改变时设置视窗大小
    virtual void resizeGL(int w, int h) override;

    // 渲染主体
    virtual void paintGL() override;

    // 鼠标事件(针对拖拽（改变视图中心）处理)
    // void mousePressEvent(QMouseEvent* event) override;
    // void mouseMoveEvent(QMouseEvent* event) override;

    //拾取
    virtual void onMousePick(const QPointF &pos);

private:
    QColor m_backgroundColor_;   // 背景颜色
    QVector3D m_viewCenter_;     // 视图中心点（世界坐标系）
    // 存在坐标系转换时每个方向转换的比例不一样的情况，暂不考虑
    float m_viewCoordFactor_;    // 世界坐标系转界面坐标系的系数

    bool m_isPickStatus_;        // 处于拾取状体

    QVector3D m_lastviewCenter_; // 记录前一次视图中心点
    float m_wheelZoomFactor_;    // 鼠标缩放系数

#if QT_VERSION_MAJOR >= 6
    void customPickMatrix(GLdouble x, GLdouble y, GLdouble width, GLdouble height, GLint viewport[4]);
#endif
};


#endif  // FIXED_PIPELINE_OPENGL_VIEW_H_

#endif  // QT_OPENGL_ES_2