#ifndef SSMAP_ROUTE_LAYER_H
#define SSMAP_ROUTE_LAYER_H

#include "base_layer.h"

#include <QGeoCoordinate>
#include <QVector>

class RouteLayer : public BaseLayer
{
    Q_OBJECT
public:
    explicit RouteLayer(QObject *parent = nullptr);

    // 设置航线数据
    void setRoute(const QVector<QGeoCoordinate> &route);
    void clearRoute();

    // 实现基类纯虚函数
    void render(QPainter* painter, 
               const QSize& viewport,
               const QGeoCoordinate& center, 
               double zoom,
               const TileForCoord::TileAlgorithm& algorithm) override;

private:
    // 航线数据
    QVector<QGeoCoordinate> m_routePoints;
    
    // 样式配置
    QColor m_lineColor = QColor(0, 120, 255);  // 航线颜色
    int m_lineWidth = 3;                       // 航线宽度
    QColor m_pointColor = QColor(255, 50, 50); // 航点颜色
    int m_pointRadius = 5;                     // 航点半径
};

#endif // SSMAP_ROUTE_LAYER_H