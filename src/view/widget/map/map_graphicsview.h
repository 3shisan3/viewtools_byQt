/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        map_graphicview.h
Version:     1.0
Author:      cjx
start date: 
Description: 地图视图控件，支持瓦片地图显示、图层管理和交互操作
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef SSMAP_GRAPHICSVIEW_H
#define SSMAP_GRAPHICSVIEW_H

#include <QGeoCoordinate>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPinchGesture>  // 添加手势识别支持
#include <QSet>

#include "view/widget/map/layers/route_layer.h"
#include "view/widget/map/layers/ship_layer.h"
#include "view/widget/map/mapengine/disk_cache_manager.h"
#include "view/widget/map/mapengine/memory_cache.h"
#include "view/widget/map/mapengine/online_tile_loader.h"

class SsMapGraphicsView : public QGraphicsView
{
    Q_OBJECT
public:
    explicit SsMapGraphicsView(QWidget* parent = nullptr);
    ~SsMapGraphicsView() override;

    // 配置项操作
    void setZoomBehavior(bool zoomAtMousePosition);
    void setTileSaveDisk(bool toAutoSave, const QString &saveDir = "");

    // 图层管理
    void addLayer(BaseLayer* layer);
    void removeLayer(BaseLayer* layer);
    void clearLayers();

    // 地图操作
    void setCenter(const QGeoCoordinate& center);
    void setZoomLevel(double zoom);
    void zoomTo(const QGeoCoordinate& center, double zoom);

    // 瓦片源配置
    void setTileAlgorithm(TileForCoord::TileAlgorithmFactory::AlgorithmType type);
    void setTileUrlTemplate(const QString& urlTemplate, const QStringList& subdomains = QStringList(), double maxLevel = -1.0);

    // 获取当前状态
    QGeoCoordinate currentCenter() const;
    double zoomLevel() const { return m_zoomLevel; }

signals:
    void curCenterChange(QGeoCoordinate pos);
    void zoomLevelChange(double level);
    void tileUrlChange(QString urlTemplate, QStringList subdomains = QStringList(), double maxLevel = 18.0);
    void tileDiskLocationChange(QString path);
    // 当前未有图层筛选功能，后续有亦可添加对应信号

protected:
    // 重写事件处理函数
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

    // 重写手势事件
    bool event(QEvent *event) override;
    void gestureEvent(QGestureEvent *event);
    void handlePinchGesture(QPinchGesture *gesture);
    // 触控点跟踪
    void handleTouchPoint(const QPointF &pos, bool isRelease = false);

private slots:
    void handleTileReceived(int x, int y, int z, const QPixmap& tile);
    void handleTileFailed(int x, int y, int z, const QString& error);
    void handleLayerUpdateRequested();

protected:
    // 坐标转换
    QPointF geoToPixel(const QGeoCoordinate& coord) const;
    QGeoCoordinate pixelToGeo(const QPointF& pixel) const;

private:
    // 视图更新
    void updateViewport();
    void requestVisibleTiles();
    
    // 瓦片管理
    void loadTile(int x, int y, int z);
    
    // 图层渲染
    void renderLayers(QPainter* painter);

    // 一些配置项
    bool m_zoomAtMousePos;  // 地图缩放基于地图中心还是鼠标所在位置
    bool m_autoSaveDisk;    // 是否自动存向磁盘
    double m_maxZoomLevel;  // 地图最大放大比例

    // 成员变量
    QGraphicsScene* m_scene;
    QGeoCoordinate m_center;
    double m_zoomLevel = 5.0;
    
    // 图层系统
    QList<BaseLayer*> m_layers;
    
    // 瓦片系统
    TileForCoord::TileAlgorithm m_tileAlgorithm;
    SsOnlineTileLoader* m_tileLoader;
    SsMemoryCache m_memoryCache;
    SsDiskCacheManager m_diskCache;
    
    // 交互状态
    QPoint m_lastMousePos;
    QSet<QString> m_requestedTiles;
    bool m_isDragging = false;
    int m_activeTouchId = -1;  // 当前激活的触控点ID
};


#endif // SSMAP_GRAPHICSVIEW_H