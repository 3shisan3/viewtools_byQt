#include "ship_layer.h"

#include <QPainter>

ShipLayer::ShipLayer(QObject *parent)
    : BaseLayer(parent)
{
}

void ShipLayer::setShipPosition(const QGeoCoordinate &position, double heading)
{
    updatePosition(position, heading);
}

void ShipLayer::updatePosition(const QGeoCoordinate &position, double heading)
{
    m_position = position;
    m_heading = heading;
    emit updateRequested();
}

void ShipLayer::render(QPainter *painter,
                       const QSize &viewport,
                       const QGeoCoordinate &center,
                       double zoom,
                       const TileForCoord::TileAlgorithm &algorithm)
{
    if (!m_position.isValid() || !isVisible())
        return;

    // 获取中心点像素坐标作为偏移基准
    QPoint centerPixel = algorithm.latLongToPixelXY(
        center.longitude(), center.latitude(), qFloor(zoom));
    QPoint viewportCenter(viewport.width() / 2, viewport.height() / 2);
    QPoint offset = viewportCenter - centerPixel;
    
    // 计算船舶位置在当前缩放级别下的像素坐标
    QPoint shipPixel = algorithm.latLongToPixelXY(
        m_position.longitude(), m_position.latitude(), qFloor(zoom));

    // 计算船舶相对于视口中心的偏移位置
    QPoint shipPos = shipPixel + offset;

    // 绘制橙色船体
    QPolygonF ship;
    ship << QPointF(0, -15) << QPointF(10, 15) << QPointF(-10, 15);

    painter->save();
    painter->translate(shipPos);
    painter->rotate(m_heading);
    painter->setBrush(QColor(255, 165, 0));
    painter->setPen(QPen(Qt::black, 1));
    painter->drawPolygon(ship);
    painter->restore();

    emit renderingComplete();
}