#ifndef SHIP_LAYER_H
#define SHIP_LAYER_H

#include "base_layer.h"
#include <QGeoCoordinate>

class ShipLayer : public BaseLayer
{
    Q_OBJECT
public:
    explicit ShipLayer(QObject *parent = nullptr);

    void setShipPosition(const QGeoCoordinate &position, double heading);
    void updatePosition(const QGeoCoordinate &position, double heading);
    
    // 实现基类纯虚函数
    void render(QPainter* painter, const QSize& viewport,
                const QGeoCoordinate& center, double zoom) override;

private:
    QGeoCoordinate m_position;
    double m_heading = 0.0;
};

#endif // SHIP_LAYER_H