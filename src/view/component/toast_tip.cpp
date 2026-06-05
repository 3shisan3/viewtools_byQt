#include "toast_tip.h"

#include <QApplication>
#include <QtGlobal>
#include <QLabel>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

SsToastTip::SsToastTip(QWidget *parent)
    : QFrame(parent)
    , m_label_(new QLabel(this))
    , m_hideTimer_(new QTimer(this))
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::ToolTip);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    m_label_->setWordWrap(true);
    m_label_->setAlignment(Qt::AlignCenter);
    m_label_->setMinimumWidth(160);
    m_label_->setMaximumWidth(420);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 10, 18, 10);
    layout->addWidget(m_label_);

    m_hideTimer_->setSingleShot(true);
    connect(m_hideTimer_, &QTimer::timeout, this, &SsToastTip::close);

    updateStyle(Info);
}

void SsToastTip::showMessage(const QString &message, TipType type, int durationMs)
{
    m_label_->setText(message);
    updateStyle(type);
    adjustSize();
    updatePosition();
    QFrame::show();
    raise();
    m_hideTimer_->start(qMax(500, durationMs));
}

void SsToastTip::showTip(QWidget *parent, const QString &message, TipType type, int durationMs)
{
    SsToastTip *toast = new SsToastTip(parent);
    toast->setAttribute(Qt::WA_DeleteOnClose, true);
    toast->showMessage(message, type, durationMs);
}

void SsToastTip::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    updatePosition();
}

void SsToastTip::updateStyle(TipType type)
{
    QString bgColor = "#333333";
    switch (type)
    {
    case Success:
        bgColor = "#2E7D32";
        break;
    case Warning:
        bgColor = "#EF6C00";
        break;
    case Error:
        bgColor = "#C62828";
        break;
    case Info:
    default:
        break;
    }

    setStyleSheet(QString(
        "SsToastTip { background: %1; border-radius: 8px; }"
        "QLabel { color: white; font-size: 13px; }").arg(bgColor));
}

void SsToastTip::updatePosition()
{
    QWidget *ownerWidget = QWidget::parentWidget();
    if (ownerWidget)
    {
        QPoint globalTopLeft = ownerWidget->mapToGlobal(QPoint(0, 0));
        int x = globalTopLeft.x() + (ownerWidget->width() - width()) / 2;
        int y = globalTopLeft.y() + 32;
        move(x, y);
        return;
    }

    QScreen *screen = QApplication::primaryScreen();
    if (!screen)
        return;

    QRect rect = screen->availableGeometry();
    move(rect.x() + (rect.width() - width()) / 2, rect.y() + 48);
}
