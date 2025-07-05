#include "joystick_wheel.h"

SsJoystickWheel::SsJoystickWheel(QWidget *parent)
    : QWidget(parent)
{
    // 初始化成员变量
    updatePosition();

    m_texturePath = {":/res/image/timer.jpg", ":/res/image/pupupu.png"};
}

SsJoystickWheel::~SsJoystickWheel()
{

}

void SsJoystickWheel::updatePosition()
{
    QPoint center{width() / 2, height() / 2};
    m_bgWheel_xy = center;
    m_rockerBar_xy = center;

    uint minLen = (width() > height() ? height() : width()) / 2;
    m_cirRadius = {minLen, minLen / 3};
}

void SsJoystickWheel::setBottomTexture(const QString &path)
{
    m_texturePath.first = path;
    update();
}

void SsJoystickWheel::setTopTexture(const QString &path)
{
    m_texturePath.second = path;
    update();
}

void SsJoystickWheel::setOuterCirRadius(uint radius)
{
    m_cirRadius.first = radius;
    update();
}

void SsJoystickWheel::setInnerCirRadius(uint radius)
{
    m_cirRadius.second = radius;
    update();
}

void SsJoystickWheel::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    // 绘图画笔
    QPainter painter(this);
    // 设置抗锯齿
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 消锯齿
    painter.setRenderHints(QPainter::SmoothPixmapTransform);

    // 绘制背景轮盘底图
    QPixmap bgCir_pixMap;
    bgCir_pixMap.load(m_texturePath.first);
    uint bigCirRadius = m_cirRadius.first;
    painter.drawPixmap(m_bgWheel_xy.x() - bigCirRadius, m_bgWheel_xy.y() - bigCirRadius,
                       bigCirRadius * 2, bigCirRadius * 2, bgCir_pixMap);
    // 绘制中间摇杆纹理
    QPixmap rocker_pixMap;
    rocker_pixMap.load(m_texturePath.second);
    uint smallCirRadius = m_cirRadius.second;
    painter.drawPixmap(m_rockerBar_xy.x() - smallCirRadius, m_rockerBar_xy.y() - smallCirRadius,
                       smallCirRadius * 2, smallCirRadius * 2, rocker_pixMap);
}

void SsJoystickWheel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePosition();
}

void SsJoystickWheel::mouseMoveEvent(QMouseEvent *event)
{

}

void SsJoystickWheel::mousePressEvent(QMouseEvent *event)
{

}

void SsJoystickWheel::mouseReleaseEvent(QMouseEvent *event)
{

}