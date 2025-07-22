#include "route_layer.h"

#include <QPainter>

RouteLayer::RouteLayer(QObject *parent)
    : BaseLayer(parent)
{
    // 初始化默认样式
}

void RouteLayer::setRoute(const QVector<QGeoCoordinate> &route)
{
    m_routePoints = route;
    emit updateRequested();
}

void RouteLayer::clearRoute()
{
    m_routePoints.clear();
    emit updateRequested();
}

void RouteLayer::render(QPainter *painter,
                        const QSize &viewport,
                        const QGeoCoordinate &center,
                        double zoom,
                        const TileForCoord::TileAlgorithm &algorithm)
{
    if (m_routePoints.isEmpty() || !isVisible())
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // 获取中心点像素坐标作为偏移基准
    QPoint centerPixel = algorithm.latLongToPixelXY(
        center.longitude(), center.latitude(), qFloor(zoom));
    QPoint viewportCenter(viewport.width() / 2, viewport.height() / 2);
    QPoint offset = viewportCenter - centerPixel;

    // 1. 绘制航线
    QPen linePen(m_lineColor, m_lineWidth);
    linePen.setCapStyle(Qt::RoundCap);
    linePen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(linePen);

    QPolygonF routePolygon;
    for (const QGeoCoordinate &coord : m_routePoints)
    {
        QPoint pixelPos = algorithm.latLongToPixelXY(
            coord.longitude(), coord.latitude(), qFloor(zoom));
        routePolygon << QPointF(pixelPos + offset);
    }
    painter->drawPolyline(routePolygon);

    // 2. 绘制航点
    painter->setPen(Qt::NoPen);
    painter->setBrush(m_pointColor);

    for (const QGeoCoordinate &coord : m_routePoints)
    {
        QPoint pixelPos = algorithm.latLongToPixelXY(
            coord.longitude(), coord.latitude(), qFloor(zoom));
        painter->drawEllipse(QPointF(pixelPos + offset), m_pointRadius, m_pointRadius);
    }

    // 3. 绘制起点和终点（特殊样式）
    if (m_routePoints.size() >= 2)
    {
        // 起点
        QPoint startPos = algorithm.latLongToPixelXY(
            m_routePoints.first().longitude(),
            m_routePoints.first().latitude(),
            qFloor(zoom));
        painter->setBrush(QColor(50, 200, 50));
        painter->drawEllipse(QPointF(startPos + offset), m_pointRadius * 1.5, m_pointRadius * 1.5);

        // 终点
        QPoint endPos = algorithm.latLongToPixelXY(
            m_routePoints.last().longitude(),
            m_routePoints.last().latitude(),
            qFloor(zoom));
        painter->setBrush(QColor(200, 50, 50));
        painter->drawEllipse(QPointF(endPos + offset), m_pointRadius * 1.5, m_pointRadius * 1.5);
    }

    painter->restore();
    emit renderingComplete();
}