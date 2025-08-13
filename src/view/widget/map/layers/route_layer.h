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

    // 获取当前航线信息
    const QVector<QGeoCoordinate> &route() const { return m_routePoints; }
    QVector<QGeoCoordinate> &route() { return m_routePoints; }

    // 设置航线数据
    void setRoute(const QVector<QGeoCoordinate> &route);
    void clearRoute();

    // 样式设置
    void setLineColor(const QColor &color) { m_lineColor = color; }
    void setLineWidth(int width) { m_lineWidth = width; }
    void setLineStyle(Qt::PenStyle style) { m_lineStyle = style; }
    void setPointColor(const QColor &color) { m_pointColor = color; }
    void setPointRadius(int radius) { m_pointRadius = radius; }

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
    QColor m_lineColor = QColor(0, 120, 255);   // 航线颜色
    int m_lineWidth = 3;                        // 航线宽度
    Qt::PenStyle m_lineStyle = Qt::SolidLine;   // 航线样式
    QColor m_pointColor = QColor(255, 50, 50);  // 航点颜色
    int m_pointRadius = 5;                      // 航点半径
};

#endif // SSMAP_ROUTE_LAYER_H