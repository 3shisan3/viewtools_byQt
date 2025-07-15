/* #include "map_display.h"

#include <QPainter>
#include <QLinearGradient>

MapDisplay::MapDisplay(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

// 地图控制接口实现
void MapDisplay::setCenter(const QGeoCoordinate& center) {
    m_mapCenter = center;
    update();
}

void MapDisplay::setZoomLevel(double level) {
    m_zoomLevel = qBound(8.0, level, 18.0);
    update();
}

// 船只跟踪接口实现
void MapDisplay::updateShipPosition(const QGeoCoordinate& position, double heading) {
    m_shipPosition = position;
    m_shipHeading = heading;
    if(m_trackingEnabled) {
        m_mapCenter = position;
    }
    update();
}

// 航线管理接口实现
void MapDisplay::addRoutePoint(const QGeoCoordinate& point) {
    m_routePoints.append(point);
    update();
}

// 核心渲染方法
void MapDisplay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // 1. 绘制基础地图
    renderBaseMap(painter);
    
    // 2. 绘制航线
    if(m_routeVisible && !m_routePoints.empty()) {
        renderRoute(painter);
    }
    
    // 3. 绘制船只
    if(m_shipPosition.isValid()) {
        renderShip(painter);
    }
    
    // 4. 绘制导航信息
    renderNavigationInfo(painter);
}

void MapDisplay::renderBaseMap(QPainter& painter) {
    // 海洋背景
    QLinearGradient oceanGrad(0, 0, 0, height());
    oceanGrad.setColorAt(0, QColor(5, 30, 60));
    oceanGrad.setColorAt(1, QColor(10, 50, 90));
    painter.fillRect(rect(), oceanGrad);
    
    // 陆地绘制 (简化版)
    QPolygonF land;
    land << QPointF(width()*0.2, height()*0.7)
         << QPointF(width()*0.4, height()*0.6)
         << QPointF(width()*0.6, height()*0.8)
         << QPointF(width()*0.3, height()*0.9);
    painter.setBrush(QColor(30, 80, 40));
    painter.drawPolygon(land);
}

void MapDisplay::renderShip(QPainter& painter) {
    QPointF shipPos = geoToPixel(m_shipPosition);
    
    // 绘制航向指示器
    painter.setPen(QPen(Qt::white, 2));
    painter.drawEllipse(shipPos, 8, 8);
    
    // 绘制船体
    QPolygonF ship;
    ship << QPointF(0, -15) << QPointF(10, 15) << QPointF(-10, 15);
    
    painter.save();
    painter.translate(shipPos);
    painter.rotate(m_shipHeading);
    painter.setBrush(QColor(200, 50, 50));
    painter.drawPolygon(ship);
    painter.restore();
}

void MapDisplay::renderRoute(QPainter& painter) {
    if(m_routePoints.size() < 2) return;
    
    QPen routePen(QColor(0, 255, 255), 3);
    routePen.setCapStyle(Qt::RoundCap);
    painter.setPen(routePen);
    
    QPainterPath path;
    path.moveTo(geoToPixel(m_routePoints.first()));
    for(int i = 1; i < m_routePoints.size(); ++i) {
        path.lineTo(geoToPixel(m_routePoints[i]));
    }
    painter.drawPath(path);
}

// 坐标转换方法
QPointF MapDisplay::geoToPixel(const QGeoCoordinate& coord) const {
    double scale = pow(2, m_zoomLevel) * 0.1;
    double x = (coord.longitude() - m_mapCenter.longitude()) * scale + width()/2;
    double y = (m_mapCenter.latitude() - coord.latitude()) * scale + height()/2;
    return QPointF(x, y);
} */