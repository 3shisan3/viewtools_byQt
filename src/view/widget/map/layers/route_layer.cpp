#include "route_layer.h"

#include <QPainter>

#include "view/widget/map/render/map_renderer.h"

RouteLayer::RouteLayer(QObject *parent) : BaseLayer(parent)
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

void RouteLayer::render(QPainter *painter, const QSize &viewport,
                       const QGeoCoordinate &center, double zoom)
{
    if (m_routePoints.isEmpty() || !isVisible())
    {
        return;
    }

    // 保存原始画笔设置
    painter->save();

    // 设置抗锯齿
    painter->setRenderHint(QPainter::Antialiasing, true);

    // 1. 绘制航线
    QPen linePen(m_lineColor, m_lineWidth);
    linePen.setCapStyle(Qt::RoundCap);
    linePen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(linePen);

    QPolygonF routePolygon;
    for (const QGeoCoordinate &coord : m_routePoints)
    {
        QPointF pixelPos = MapRenderer::geoToPixel(coord, center, zoom, viewport);
        routePolygon << pixelPos;
    }
    painter->drawPolyline(routePolygon);

    // 2. 绘制航点
    painter->setPen(Qt::NoPen);
    painter->setBrush(m_pointColor);

    for (const QGeoCoordinate &coord : m_routePoints)
    {
        QPointF p = MapRenderer::geoToPixel(coord, center, zoom, viewport);
        painter->drawEllipse(p, m_pointRadius, m_pointRadius);
    }

    // 3. 绘制起点和终点（特殊样式）
    if (m_routePoints.size() >= 2)
    {
        // 起点
        QPointF startPos = MapRenderer::geoToPixel(m_routePoints.first(), center, zoom, viewport);
        painter->setBrush(QColor(50, 200, 50)); // 绿色起点
        painter->drawEllipse(startPos, m_pointRadius * 1.5, m_pointRadius * 1.5);

        // 终点
        QPointF endPos = MapRenderer::geoToPixel(m_routePoints.last(), center, zoom, viewport);
        painter->setBrush(QColor(200, 50, 50)); // 红色终点
        painter->drawEllipse(endPos, m_pointRadius * 1.5, m_pointRadius * 1.5);
    }

    // 恢复原始画笔设置
    painter->restore();

    emit renderingComplete();
}