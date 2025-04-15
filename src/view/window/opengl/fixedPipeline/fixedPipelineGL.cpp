#include "fixedPipelineGL.h"

#include <cmath>
#include <GL/glu.h>
#include <QPainter>

#define PICKSIZE 1024
#define PICK_TOL 8                                                  // 拾取阈值（可读取配置文件，依据当前绘制要素，动态变化）

SsFixedPipelineGLWidgetBase::SsFixedPipelineGLWidgetBase(QWidget * parent)
    : QOpenGLWidget(parent)
    , m_viewCenter_(0.0f, 0.0f, 0.0f)                               // 初始中心点为 (0,0)
    , m_viewCoordFactor_(1.0f)                                      // 初始系数为1，和逻辑坐标系（QPointF)比例直接对应
    , m_isPickStatus_(false)
{
    // 在构造函数中设置 OpenGL 上下文格式
    QSurfaceFormat format;
    format.setVersion(2, 1);                                        // OpenGL 2.1
    format.setProfile(QSurfaceFormat::CompatibilityProfile);        // 兼容模式
    format.setSamples(4);                                           // 启用4x MSAA
    setFormat(format);                                              // 关键：为当前 widget 设置格式


}

SsFixedPipelineGLWidgetBase::~SsFixedPipelineGLWidgetBase()
{

}

void SsFixedPipelineGLWidgetBase::initializeGL()
{
    // 初始化 OpenGL 函数
    initializeOpenGLFunctions();
    // 设置背景色
    glClearColor(m_backgroundColor_.redF(), m_backgroundColor_.greenF(), m_backgroundColor_.blueF(), 1.0f);

    // 开启深度测试&指定深度测试算法（这样绘制不会依赖先后顺序）
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);                                         // 默认值为GL_LESS，在开启反走样后无法正常的对混合后的锯齿进行深度测试

    // 开混合模式贴图&指定混合算法（反走样依赖该选项，半透明纹理的正常显示也依赖该选项）
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnable(GL_MULTISAMPLE);
}

void SsFixedPipelineGLWidgetBase::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h); // 设置视口

    // 计算映射到世界坐标系下宽高（保持宽高比）
    float width = w / m_viewCoordFactor_;
    float height = h / m_viewCoordFactor_;

    // 计算投影矩阵边界
    float left = m_viewCenter_.x() - width / 2;
    float right = m_viewCenter_.x() + width / 2;
    float bottom = m_viewCenter_.y() - height / 2;
    float top = m_viewCenter_.y() + height / 2;

    glMatrixMode(GL_PROJECTION);                                    // 切换投影矩阵，用于后续设置视图的投影方式（如正交投影或透视投影）
    glLoadIdentity();
    glOrtho(left, right, bottom, top, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);                                     // 切换模型视图矩阵，用于设置物体的变换（如平移、旋转、缩放）
}

void SsFixedPipelineGLWidgetBase::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glLoadIdentity();                                               // 重置为单位矩阵

    /* 添加绘制代码 */
}

void SsFixedPipelineGLWidgetBase::setBackgroundColor(const QColor &color)
{
    m_backgroundColor_ = color;
}

void SsFixedPipelineGLWidgetBase::setViewCenter(const QVector3D &pos)
{
    m_viewCenter_ = pos;
}

void SsFixedPipelineGLWidgetBase::setCoordFactor(const float value)
{
    m_viewCoordFactor_ = value;
}

QPointF SsFixedPipelineGLWidgetBase::ndcTrans(const QVector3D &point)
{
    float offsetX = point.x() - m_viewCenter_.x();
    float offsetY = point.y() - m_viewCenter_.y();

    QPointF pt(this->width() / 2 + offsetX * m_viewCoordFactor_,
               this->height() / 2 + offsetY * m_viewCoordFactor_);
    return pt;
}

QVector3D SsFixedPipelineGLWidgetBase::ndcTrans(const QPointF &point)
{
    float offsetX = point.x() - this->width() / 2;
    float offsetY = point.y() - this->height() / 2;

    QVector3D pt(m_viewCenter_.x() + offsetX / m_viewCoordFactor_,
                 m_viewCenter_.y() + offsetY / m_viewCoordFactor_,
                 m_viewCenter_.z());
    return pt;
}

void SsFixedPipelineGLWidgetBase::setViewRect(const QVector3D &topLeft, const QVector3D &bottomRight)
{
    m_viewCoordFactor_ = std::fabs(bottomRight.x() - topLeft.x()) / this->width();

    m_viewCenter_.setX((bottomRight.x() + topLeft.x()) / 2);
    m_viewCenter_.setY((topLeft.y() + bottomRight.y()) / 2);
    m_viewCenter_.setZ((topLeft.z() + bottomRight.z()) / 2);
}

bool SsFixedPipelineGLWidgetBase::isPickMode() const
{
    return m_isPickStatus_;
}

void SsFixedPipelineGLWidgetBase::drawPoint(const QVector3D &point, float size, const QColor &color)
{
    glPointSize(size);

    glBegin(GL_POINTS);
        glColor3f(color.redF(), color.greenF(), color.blueF());
        glVertex3f(point.x(), point.y(), point.z());
    glEnd();
}

void SsFixedPipelineGLWidgetBase::drawLine(const QVector3D &start, const QVector3D &end, float width, const QColor &color)
{
    glLineWidth(width);
    glBegin(GL_LINES);
        glColor3f(color.redF(), color.greenF(), color.blueF());
        glVertex3f(start.x(), start.y(), start.z());
        glVertex3f(end.x(), end.y(), end.z());
    glEnd();
}

void SsFixedPipelineGLWidgetBase::drawPolygon(const QVector<QVector3D> &points, const QColor &borderColor, const QColor &fillColor)
{
    if (borderColor != Qt::transparent)
    {
        glLineWidth(1.0f);
        glBegin(GL_LINE_LOOP);
        glColor3f(borderColor.redF(), borderColor.greenF(), borderColor.blueF());
        for (const QVector3D &point : points)
        {
            glVertex3f(point.x(), point.y(), point.z());
        }
        glEnd();
    }

    if (fillColor != Qt::transparent)
    {
        glBegin(GL_POLYGON);
        glColor3f(fillColor.redF(), fillColor.greenF(), fillColor.blueF());
        for (const QVector3D &point : points)
        {
            glVertex3f(point.x(), point.y(), point.z());
        }
        glEnd();
    }
}

void SsFixedPipelineGLWidgetBase::drawText(const QPointF &position, const QString &text, const QColor &color, int fontSize)
{
    // 使用 QPainter 绘制文字
    QPainter painter(this);

    painter.setCompositionMode(QPainter::CompositionMode_Source); // 直接覆盖，不混合
    painter.setBrush(Qt::transparent); // 若需要透明背景

    painter.setPen(color);
    painter.setFont(QFont("Arial", fontSize));
    painter.drawText(position.toPoint(), text);
    painter.end();
}

void SsFixedPipelineGLWidgetBase::drawTexture(const QVector2D &position, const QSize &size, QOpenGLTexture *texture)
{
    if (!texture || !texture->isCreated()) {
        return;  // 纹理无效
    }

    // 绑定纹理
    texture->bind(); // glBindTexture(GL_TEXTURE_2D, texture->textureId());

    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
        glColor3f(1.0f, 1.0f, 1.0f); // 纹理颜色不叠加
        glTexCoord2f(0, 0); glVertex2f(position.x(), position.y());                                 // 左下
        glTexCoord2f(1, 0); glVertex2f(position.x() + size.width(), position.y());                  // 右下
        glTexCoord2f(1, 1); glVertex2f(position.x() + size.width(), position.y() + size.height());  // 右上
        glTexCoord2f(0, 1); glVertex2f(position.x(), position.y() + size.height());                 // 左上
    glEnd();

    glDisable(GL_TEXTURE_2D);
}

void SsFixedPipelineGLWidgetBase::onMousePick(const QPointF &pos)
{
    /* 初始化阶段 */
    GLuint pickBuffer[PICKSIZE] = {0};              // 创建一个保存选择结果的数组
    GLint numPicks = 0;
    GLint vp[4] = {0};
    
    glGetIntegerv(GL_VIEWPORT, vp);                 // 获取当前视口参数(vp[2]=width, vp[3]=height)
    glSelectBuffer(PICKSIZE, pickBuffer);           // 设置选择缓冲区

    // 复用 resizeGL 中的投影计算逻辑
    float width = vp[2] / m_viewCoordFactor_;
    float height = vp[3] / m_viewCoordFactor_;
    float left = m_viewCenter_.x() - width / 2;
    float right = m_viewCenter_.x() + width / 2;
    float bottom = m_viewCenter_.y() - height / 2;
    float top = m_viewCenter_.y() + height / 2;

    /* 选择模式配置 */
    glRenderMode(GL_SELECT);                        // 激活选择模式
    glInitNames();                                  // 初始化名字堆栈
    glPushName(0);                                  // 在名字栈中放入一个初始化名字，这里为‘0’ (防止空栈错误)

    m_isPickStatus_ = true;

    /* 投影矩阵操作 */
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();                                 // 保存原有投影矩阵
    glLoadIdentity();

    glOrtho(left, right, bottom, top, -1.0, 1.0);   // 先应用原始投影矩阵
    gluPickMatrix(GLdouble(pos.x()), GLdouble(vp[3] - pos.y()), PICK_TOL, PICK_TOL, vp);    // 叠加拾取矩阵

    /* 绘制场景 */
    paintGL();                                      // 触发绘制（夹取的一帧）(update()同样)

    /* 结果处理 */
    glMatrixMode(GL_PROJECTION);                    // 恢复矩阵
    glPopMatrix();                                  // 返回正常的投影变换 

    numPicks = glRenderMode(GL_RENDER);             // 从选择模式返回正常模式,该函数返回选择到对象的个数
    m_isPickStatus_ = false;

    /* 拾取结果pickBuffer解析，通过glPushName存入的名字来映射到具体要素 */
    /* // 示例：两个命中记录
        pickBuffer[] = {
            // 第一个命中记录
            2,          // 名称堆栈深度（n=2）
            minDepth1,  // 最小深度
            maxDepth1,  // 最大深度
            name1_1,    // 名称1（堆栈底部名称）
            name1_2,    // 名称2（堆栈顶部名称）

            // 第二个命中记录
            1,          // 名称堆栈深度（n=1）
            minDepth2,  // 最小深度
            maxDepth2,  // 最大深度
            name2       // 名称
        };
     */
}