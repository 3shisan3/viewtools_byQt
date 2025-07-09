#include "circle_button.h"

#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionButton>

SsCircleButton::SsCircleButton(QWidget *parent)
    : QPushButton(parent)
{
    init();
}

SsCircleButton::SsCircleButton(const QString &text, QWidget *parent)
    : QPushButton(text, parent)
{
    init();
}

void SsCircleButton::init()
{
    // 保持与普通按钮相同的策略
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

    // 启用悬停和按压状态
    setAttribute(Qt::WA_Hover);
}

void SsCircleButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 创建圆形路径
    QPainterPath path;
    path.addEllipse(rect().adjusted(1, 1, -1, -1)); // 稍微缩小1像素避免边缘裁剪

    // 应用圆形裁剪
    p.setClipPath(path);

    // 使用标准按钮样式绘制背景
    QStyleOptionButton option;
    initStyleOption(&option);
    // 委托给QStyle绘制（保持平台原生样式）
    style()->drawControl(QStyle::CE_PushButton, &option, &p, this);

    // 如果需要自定义文本位置可以在此添加
    if (!text().isEmpty())
    {
        p.setPen(palette().buttonText().color());
        p.drawText(rect(), Qt::AlignCenter, text());
    }
}

void SsCircleButton::resizeEvent(QResizeEvent *event)
{
    // 保持宽高相同
    int size = qMin(event->size().width(), event->size().height());
    if (size != event->size().width() || size != event->size().height())
    {
        resize(size, size);
    }
    else
    {
        QPushButton::resizeEvent(event);
    }
}

bool SsCircleButton::hitButton(const QPoint &pos) const
{
    // 只在圆形区域内响应点击
    QPoint center = rect().center();
    return QVector2D(pos - center).length() <= (width() / 2);
}