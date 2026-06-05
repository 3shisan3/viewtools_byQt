#include "status_badge.h"

SsStatusBadge::SsStatusBadge(QWidget *parent)
    : QLabel(parent)
    , m_status_(Normal)
{
    setAlignment(Qt::AlignCenter);
    setMinimumHeight(24);
    setContentsMargins(10, 3, 10, 3);
    setStatus(Normal, tr("Normal"));
}

void SsStatusBadge::setStatus(Status status, const QString &text)
{
    m_status_ = status;
    if (!text.isEmpty())
        setText(text);

    refreshStyle();
}

void SsStatusBadge::refreshStyle()
{
    QString bgColor;
    QString textColor = "#FFFFFF";

    switch (m_status_)
    {
    case Running:
        bgColor = "#1976D2";
        break;
    case Warning:
        bgColor = "#EF6C00";
        break;
    case Error:
        bgColor = "#C62828";
        break;
    case Disabled:
        bgColor = "#9E9E9E";
        break;
    case Normal:
    default:
        bgColor = "#2E7D32";
        break;
    }

    setStyleSheet(QString(
        "QLabel { background: %1; color: %2; border-radius: 12px; padding: 3px 10px; }"
    ).arg(bgColor, textColor));
}
