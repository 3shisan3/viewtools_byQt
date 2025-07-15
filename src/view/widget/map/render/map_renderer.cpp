#include "map_renderer.h"
#include <cmath>

MapRenderer::MapRenderer(QObject *parent) : QObject(parent) {}

void MapRenderer::setViewportSize(const QSize &size)
{
    m_viewport = size;
}

void MapRenderer::setCenter(const QGeoCoordinate &center)
{
    m_center = center;
}

void MapRenderer::setZoomLevel(double zoom)
{
    m_zoom = qBound(3.0, zoom, 18.0);
}

// 实际计算实现（不包含视口偏移）
QPointF MapRenderer::rawGeoToPixel(const QGeoCoordinate &coord) const
{
    double scale = 256 * pow(2, m_zoom);
    double x = (coord.longitude() + 180.0) / 360.0 * scale;
    double y = (1.0 - log(tan(coord.latitude() * M_PI / 180.0) + 
              1.0 / cos(coord.latitude() * M_PI / 180.0)) / M_PI) / 2.0 * scale;
    return QPointF(x, y);
}

// 非递归版本（供内部使用）
QPointF MapRenderer::geoToPixelNoRecurse(const QGeoCoordinate &coord) const
{
    QPointF point = rawGeoToPixel(coord);
    QPointF center = rawGeoToPixel(m_center);
    return QPointF(point.x() - center.x() + m_viewport.width()/2,
                   point.y() - center.y() + m_viewport.height()/2);
}

// 对外接口（保持原有签名）
QPointF MapRenderer::geoToPixel(const QGeoCoordinate &coord) const
{
    return geoToPixelNoRecurse(coord);
}

QGeoCoordinate MapRenderer::pixelToGeo(const QPointF &point) const
{
    double scale = 256 * pow(2, m_zoom);

    QPointF centerPx = geoToPixel(m_center);
    double x = point.x() + centerPx.x() - m_viewport.width() / 2;
    double y = point.y() + centerPx.y() - m_viewport.height() / 2;

    double lon = (x / scale) * 360.0 - 180.0;
    double latRad = atan(sinh(M_PI * (1 - 2 * y / scale)));
    double lat = latRad * 180.0 / M_PI;

    return QGeoCoordinate(lat, lon);
}

// 静态原始坐标转换
QPointF MapRenderer::staticRawGeoToPixel(const QGeoCoordinate &coord, double zoom)
{
    double scale = 256 * pow(2, zoom);
    double x = (coord.longitude() + 180.0) / 360.0 * scale;
    double latRad = coord.latitude() * M_PI / 180.0;
    double y = (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) / 2.0 * scale;
    return QPointF(x, y);
}

// 添加静态方法供RouteLayer使用
QPointF MapRenderer::geoToPixel(const QGeoCoordinate &coord, const QGeoCoordinate &center, double zoom, const QSize &viewport)
{
    QPointF point = staticRawGeoToPixel(coord, zoom);
    QPointF centerPx = staticRawGeoToPixel(center, zoom); // 直接计算，不递归
    
    return QPointF(
        point.x() - centerPx.x() + viewport.width() / 2,
        point.y() - centerPx.y() + viewport.height() / 2
    );
}