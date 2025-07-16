#pragma once

#include <QWidget>
#include <QGeoCoordinate>

#include "view/widget/map/layers/ship_layer.h"
#include "view/widget/map/layers/route_layer.h"
#include "view/widget/map/mapengine/online_tile_loader.h"
#include "view/widget/map/mapengine/disk_cache_manager.h"
#include "view/widget/map/mapengine/memory_cache.h"
#include "view/widget/map/render/map_renderer.h"

class MarineMapComponent : public QWidget {
    Q_OBJECT
public:
    explicit MarineMapComponent(QWidget* parent = nullptr);
    
    // 地图控制
    void setCenter(const QGeoCoordinate& center);
    void setZoom(double zoom);
    void setMapStyle(const QString& styleUrl);
    
    // 船舶数据
    void updateShipData(const QGeoCoordinate& position, double heading);
    
    // 航线管理
    void setRoute(const QVector<QGeoCoordinate>& route);
    void clearRoute();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
    void handleTileReceived(int x, int y, int z, const QPixmap& tile);

private:
    // 引擎组件
    SsOnlineTileLoader* m_tileLoader;
    SsDiskCacheManager* m_diskCache;
    SsMemoryCache* m_memoryCache;
    
    // 渲染系统
    MapRenderer* m_renderer;
    
    // 图层系统
    ShipLayer* m_shipLayer;
    RouteLayer* m_routeLayer;
    
    // 交互状态
    QPoint m_lastMousePos;
    bool m_isDragging = false;
};