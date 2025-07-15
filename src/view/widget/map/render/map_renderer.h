#pragma once

#include <QGeoCoordinate>
#include <QObject>
#include <QPointF>
#include <QSize>

class MapRenderer : public QObject
{
    Q_OBJECT
public:
    explicit MapRenderer(QObject *parent = nullptr);

    void setViewportSize(const QSize &size);
    void setCenter(const QGeoCoordinate &center);
    void setZoomLevel(double zoom);

    QPointF geoToPixel(const QGeoCoordinate &coord) const;
    QPointF geoToPixelNoRecurse(const QGeoCoordinate &coord) const;
    QGeoCoordinate pixelToGeo(const QPointF &point) const;

    double zoomLevel() const { return m_zoom; }
    QGeoCoordinate center() const { return m_center; }

    static QPointF geoToPixel(const QGeoCoordinate &coord, const QGeoCoordinate &center, double zoom, const QSize &viewport);

private:
    QPointF rawGeoToPixel(const QGeoCoordinate &coord) const;
    
    static QPointF staticRawGeoToPixel(const QGeoCoordinate &coord, double zoom);

private:
    QSize m_viewport;
    QGeoCoordinate m_center;
    double m_zoom = 10.0;
};