#include "ship_layer.h"

#include <QPainter>

#include "view/widget/map/render/map_renderer.h"

ShipLayer::ShipLayer(QObject *parent) : BaseLayer(parent) {}

void ShipLayer::setShipPosition(const QGeoCoordinate &position, double heading)
{
    updatePosition(position, heading);
}

void ShipLayer::updatePosition(const QGeoCoordinate &position, double heading)
{
    m_position = position;
    m_heading = heading;
    emit renderingComplete();
}

void ShipLayer::render(QPainter *painter, const QSize &viewport,
                       const QGeoCoordinate &center, double zoom)
{
    if (!m_position.isValid())
        return;

    QPointF shipPos = MapRenderer::geoToPixel(m_position, center, zoom, viewport);

    // 绘制橙色船体 (根据图片要求)
    QPolygonF ship;
    ship << QPointF(0, -15) << QPointF(10, 15) << QPointF(-10, 15);

    painter->save();
    painter->translate(shipPos);
    painter->rotate(m_heading);
    painter->setBrush(QColor(255, 165, 0)); // 橙色
    painter->setPen(QPen(Qt::black, 1));
    painter->drawPolygon(ship);
    painter->restore();
}