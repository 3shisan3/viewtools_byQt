#ifndef SSMAP_GRAPHICSVIEW_H
#define SSMAP_GRAPHICSVIEW_H

#include <QGeoCoordinate>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QHash>
#include "view/widget/map/layers/route_layer.h"
#include "view/widget/map/layers/ship_layer.h"
#include "view/widget/map/mapengine/disk_cache_manager.h"
#include "view/widget/map/mapengine/memory_cache.h"
#include "view/widget/map/mapengine/online_tile_loader.h"

class MapTileItem;

class SsMapGraphicsView : public QGraphicsView
{
    Q_OBJECT
public:
    explicit SsMapGraphicsView(QWidget* parent = nullptr);
    ~SsMapGraphicsView();

    // 地图控制
    void setCenter(const QGeoCoordinate& center);
    QGeoCoordinate center() const { return m_center; }
    void setZoom(double zoom);
    double zoom() const { return m_zoomLevel; }
    void setMapStyle(const QString& styleUrl);
    
    // 图层管理
    void addLayer(BaseLayer* layer);
    void removeLayer(BaseLayer* layer);
    
    // 船舶数据
    void updateShipData(const QGeoCoordinate& position, double heading);
    
    // 航线管理
    void setRoute(const QVector<QGeoCoordinate>& route);
    void clearRoute();

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void handleTileReceived(int x, int y, int z, const QPixmap& tile);
    void updateSceneRect(const QRectF& rect);

private:
    // 坐标转换方法
    QPointF geoToScene(const QGeoCoordinate& geo) const;
    QGeoCoordinate sceneToGeo(const QPointF& scenePos) const;
    void sceneToTile(const QPointF& scenePos, int& x, int& y) const;
    
    // 瓦片管理
    void loadVisibleTiles();
    void clearAllTiles();
    MapTileItem* getTileItem(int x, int y, int z);
    void updateMapTiles();

    // 场景和图层管理
    QGraphicsScene* m_scene;
    QHash<QString, MapTileItem*> m_tileItems; // 使用 "x-y-z" 作为键
    QList<QPair<BaseLayer*, QGraphicsItem*>> m_layers;
    
    // 地图状态
    QGeoCoordinate m_center;
    double m_zoomLevel = 10.0;
    int m_tileSize = 256;
    
    // 交互状态
    QPoint m_lastMousePos;
    bool m_isDragging = false;
    
    // 瓦片加载和缓存
    SsOnlineTileLoader* m_tileLoader;
    SsDiskCacheManager* m_diskCache;
    SsMemoryCache* m_memoryCache;
};

// 瓦片图元类
class MapTileItem : public QGraphicsItem
{
public:
    MapTileItem(int x, int y, int z, const QPixmap& pixmap, QGraphicsItem* parent = nullptr);
    
    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    
    int x() const { return m_x; }
    int y() const { return m_y; }
    int z() const { return m_z; }
    
private:
    int m_x, m_y, m_z;
    QPixmap m_pixmap;
};

// 图层图元类
class MapLayerItem : public QGraphicsItem
{
public:
    MapLayerItem(BaseLayer* layer, SsMapGraphicsView* mapView, QGraphicsItem* parent = nullptr);
    
    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    
protected:
    BaseLayer* m_layer;
    SsMapGraphicsView* m_mapView;
    QRectF m_cachedRect;
};

#endif // SSMAP_GRAPHICSVIEW_H