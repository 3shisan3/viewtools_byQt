/* #pragma once

#include <QGeoCoordinate>
#include <QVector>

class MapDisplay : public QWidget
{
    Q_OBJECT
public:
    explicit MapDisplay(QWidget *parent = nullptr);

    // 地图控制接口
    void setCenter(const QGeoCoordinate &center);
    void setZoomLevel(double level);
    void setMapStyle(const QString &style);

    // 船只跟踪接口
    void updateShipPosition(const QGeoCoordinate &position, double heading);
    void setShipTracking(bool enabled);

    // 航线管理接口
    void addRoutePoint(const QGeoCoordinate &point);
    void clearRoute();
    void setRouteVisible(bool visible);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    // 地图渲染核心方法
    void renderBaseMap(QPainter &painter);
    void renderShip(QPainter &painter);
    void renderRoute(QPainter &painter);
    void renderNavigationInfo(QPainter &painter);

    // 坐标转换
    QPointF geoToPixel(const QGeoCoordinate &coord) const;
    QGeoCoordinate pixelToGeo(const QPointF &point) const;

    // 地图状态
    QGeoCoordinate m_mapCenter{21.48341372, 109.05621073};
    double m_zoomLevel = 12.0;
    QString m_mapStyle = "default";

    // 船只状态
    QGeoCoordinate m_shipPosition;
    double m_shipHeading = 23.5;
    bool m_trackingEnabled = true;

    // 航线数据
    QVector<QGeoCoordinate> m_routePoints;
    bool m_routeVisible = true;
}; */