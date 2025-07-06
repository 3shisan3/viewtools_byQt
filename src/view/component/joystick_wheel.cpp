#include "joystick_wheel.h"

#if QT_VERSION_MAJOR >= 6
    using TouchDeviceType = QPointingDevice;
#else
    using TouchDeviceType = QTouchDevice;
#endif

SsJoystickWheel::SsJoystickWheel(QWidget *parent)
    : QWidget(parent)
    , m_hasPressed(false)
    , m_activeTouchId(-1)
{
    if (TouchDeviceType::devices().count() > 0) // 判断是否为触控设备
        setAttribute(Qt::WA_AcceptTouchEvents); // 启用触摸事件

    // 初始化成员变量
    updatePosition();

    m_texture = {QPixmap(":/res/image/timer.jpg"), QPixmap(":/res/image/pupupu.png")};
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
    m_texture.first.load(path);
    update();
}

void SsJoystickWheel::setTopTexture(const QString &path)
{
    m_texture.second.load(path);
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

bool SsJoystickWheel::event(QEvent *event)
{
    switch (event->type())
    {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
    {
        QTouchEvent *touchEvent = static_cast<QTouchEvent *>(event);
#if QT_VERSION_MAJOR >= 6
        const auto &touchPoints = touchEvent->points();
#else
        const auto &touchPoints = touchEvent->touchPoints();
#endif

        // 处理激活中的触控点（优先）
        if (m_activeTouchId != -1)
        {
            for (const auto &point : touchPoints)
            {
                if (point.id() == m_activeTouchId)
                {
#if QT_VERSION_MAJOR >= 6
                    handleTouchPoint(point.position(), event->type() == QEvent::TouchEnd);
#else
                    handleTouchPoint(point.pos(), event->type() == QEvent::TouchEnd);
#endif
                    if (event->type() == QEvent::TouchEnd)
                    {
                        m_activeTouchId = -1; // 释放激活点
                    }
                    return true;
                }
            }
            // 如果激活点已消失（如被系统取消）
            m_activeTouchId = -1;
        }

        // 寻找新的有效触控点（仅TouchBegin时）
        if (event->type() == QEvent::TouchBegin)
        {
            for (const auto &point : touchPoints)
            {
#if QT_VERSION_MAJOR >= 6
                auto p = point.position();
#else
                auto p = point.pos();
#endif              

                double dx = p.x() - m_bgWheel_xy.x();
                double dy = p.y() - m_bgWheel_xy.y();
                if (dx * dx + dy * dy <= m_cirRadius.first * m_cirRadius.first)
                {
                    m_activeTouchId = point.id();
                    handleTouchPoint(p);
                    return true;
                }
            }
        }
        return true;
    }
    default:
        return QWidget::event(event);
    }
}

void SsJoystickWheel::handleTouchPoint(const QPointF &pos, bool isRelease)
{
    if (isRelease)
    {
        // 摇杆回弹到中心
        m_rockerBar_xy = m_bgWheel_xy;
    }
    else
    {
        // 计算摇杆位置（限制在外圆内）
        double dx = pos.x() - m_bgWheel_xy.x();
        double dy = pos.y() - m_bgWheel_xy.y();
        double distance = sqrt(dx * dx + dy * dy);
        double maxDistance = m_cirRadius.first;

        if (distance <= maxDistance)
        {
            m_rockerBar_xy = pos.toPoint();
        }
        else
        {
            double ratio = maxDistance / distance;
            m_rockerBar_xy.setX(m_bgWheel_xy.x() + dx * ratio);
            m_rockerBar_xy.setY(m_bgWheel_xy.y() + dy * ratio);
        }
    }
    update(); // 触发重绘
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
    uint bigCirRadius = m_cirRadius.first;
    painter.drawPixmap(m_bgWheel_xy.x() - bigCirRadius, m_bgWheel_xy.y() - bigCirRadius,
                       bigCirRadius * 2, bigCirRadius * 2, m_texture.first);
    // 绘制中间摇杆纹理
    uint smallCirRadius = m_cirRadius.second;
    painter.drawPixmap(m_rockerBar_xy.x() - smallCirRadius, m_rockerBar_xy.y() - smallCirRadius,
                       smallCirRadius * 2, smallCirRadius * 2, m_texture.second);
}

void SsJoystickWheel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePosition();
}

void SsJoystickWheel::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_hasPressed)
        return;

    // 获取鼠标操作位置
    QPoint mousePos = event->pos();
    // 判断是否超出最大范围
    double distance = sqrt(pow(mousePos.x() - m_bgWheel_xy.x(), 2) + pow(mousePos.y() - m_bgWheel_xy.y(), 2));
    double maxDis = m_cirRadius.first;
    if (distance <= maxDis)
    {
        m_rockerBar_xy = mousePos;
    }
    else
    {
        // 获取偏移角度
        double angle = atan2(mousePos.y() - m_bgWheel_xy.y(), mousePos.x() - m_bgWheel_xy.x());
        m_rockerBar_xy = QPoint(
                m_bgWheel_xy.x() + maxDis * cos(angle),
                m_bgWheel_xy.y() + maxDis * sin(angle)
            );
    }

    update();
}

void SsJoystickWheel::mousePressEvent(QMouseEvent *event)
{
    if (m_activeTouchId != -1) return; // 触摸优先

    // 获取鼠标操作位置
    QPoint mousePos = event->pos();

    // 计算处理位置是否在大圆内
    if (std::pow((mousePos.x() - m_bgWheel_xy.x()), 2) +
        std::pow((mousePos.y() - m_bgWheel_xy.y()), 2) <=
        std::pow(m_cirRadius.first, 2))
    {
        m_hasPressed = true;
    }
}

void SsJoystickWheel::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);

    m_hasPressed = false;
    // 回弹小圆中心
    m_rockerBar_xy.setX(m_bgWheel_xy.x());
    m_rockerBar_xy.setY(m_bgWheel_xy.y());

    update();
}