#pragma once
#include "base_layer.h"
#include <QGeoCoordinate>

class ShipLayer : public BaseLayer
{
    Q_OBJECT
public:
    explicit ShipLayer(QObject *parent = nullptr);

    void setShipPosition(const QGeoCoordinate &position, double heading);
    void updatePosition(const QGeoCoordinate &position, double heading); // 添加这个方法
    void render(QPainter *painter, const QSize &viewport,
                const QGeoCoordinate &center, double zoom) override;

private:
    QGeoCoordinate m_position;
    double m_heading = 0.0;
};