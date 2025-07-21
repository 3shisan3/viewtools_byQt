#include "map_graphicsview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStandardPaths>
#include <QtMath>
#include <QWheelEvent>

// MapTileItem 实现
MapTileItem::MapTileItem(int x, int y, int z, const QPixmap& pixmap, QGraphicsItem* parent)
    : QGraphicsItem(parent), m_x(x), m_y(y), m_z(z), m_pixmap(pixmap)
{
    setPos(x * 256, y * 256);
    setZValue(-1); // 确保瓦片在底层
}

QRectF MapTileItem::boundingRect() const
{
    return QRectF(0, 0, m_pixmap.width(), m_pixmap.height());
}

void MapTileItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*)
{
    painter->drawPixmap(0, 0, m_pixmap);
}

// MapLayerItem 实现
MapLayerItem::MapLayerItem(BaseLayer* layer, SsMapGraphicsView* mapView, QGraphicsItem* parent)
    : QGraphicsItem(parent), m_layer(layer), m_mapView(mapView)
{
    if (mapView && mapView->scene()) {
        m_cachedRect = mapView->scene()->sceneRect();
        connect(mapView->scene(), &QGraphicsScene::sceneRectChanged, 
               this, [this](const QRectF& rect) {
                   prepareGeometryChange();
                   m_cachedRect = rect;
                   update();
               });
    }
    setFlag(QGraphicsItem::ItemHasNoContents, false);
}

QRectF MapLayerItem::boundingRect() const
{
    return m_cachedRect.isValid() ? m_cachedRect : QRectF(0, 0, 100, 100);
}

void MapLayerItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*)
{
    if (m_layer && m_layer->isVisible() && m_mapView && m_mapView->viewport()) {
        QSize viewportSize = m_mapView->viewport()->size();
        painter->save();
        m_layer->render(painter, viewportSize, m_mapView->center(), m_mapView->zoom());
        painter->restore();
    }
}

// SsMapGraphicsView 实现
SsMapGraphicsView::SsMapGraphicsView(QWidget* parent)
    : QGraphicsView(parent),
      m_scene(new QGraphicsScene(this))
{
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    
    // 初始化缓存系统
    QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/marine_tiles";
    m_diskCache = new SsDiskCacheManager(cachePath, this);
    m_memoryCache = new SsMemoryCache(100); // 100MB内存缓存
    
    // 初始化瓦片加载器
    m_tileLoader = new SsOnlineTileLoader(this);
    m_tileLoader->setUrlTemplate("https://webst02.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}");
    connect(m_tileLoader, &SsOnlineTileLoader::tileReceived, this, &SsMapGraphicsView::handleTileReceived);
    
    // 初始位置
    setCenter(QGeoCoordinate(21.48341372, 109.05621073));
    setZoom(12);
    
    // 初始场景矩形
    updateSceneRect(QRectF(0, 0, 4096, 4096));
}

SsMapGraphicsView::~SsMapGraphicsView()
{
    clearAllTiles();
    qDeleteAll(m_layers);
}

void SsMapGraphicsView::addLayer(BaseLayer* layer)
{
    if (!layer) return;
    
    auto* layerItem = new MapLayerItem(layer, this);
    m_scene->addItem(layerItem);
    m_layers.append(qMakePair(layer, layerItem));
    
    connect(layer, &BaseLayer::updateRequested, this, [this, layerItem]() {
        if (layerItem) layerItem->update();
    });
}

void SsMapGraphicsView::removeLayer(BaseLayer* layer)
{
    for (int i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i].first == layer) {
            m_scene->removeItem(m_layers[i].second);
            delete m_layers[i].second;
            m_layers.removeAt(i);
            break;
        }
    }
}

void SsMapGraphicsView::setCenter(const QGeoCoordinate& center)
{
    if (m_center != center) {
        m_center = center;
        updateMapTiles();
    }
}

void SsMapGraphicsView::setZoom(double zoom)
{
    zoom = qBound(3.0, zoom, 18.0);
    if (!qFuzzyCompare(m_zoomLevel, zoom)) {
        m_zoomLevel = zoom;
        updateMapTiles();
    }
}

void SsMapGraphicsView::setMapStyle(const QString& styleUrl)
{
    m_tileLoader->setUrlTemplate(styleUrl);
    // 清除缓存以加载新样式
    m_memoryCache->clear();
    m_diskCache->clearCache();
    clearAllTiles();
    updateMapTiles();
}

void SsMapGraphicsView::updateShipData(const QGeoCoordinate& position, double heading)
{
    for (auto& layerPair : m_layers) {
        if (auto* shipLayer = qobject_cast<ShipLayer*>(layerPair.first)) {
            shipLayer->setShipPosition(position, heading);
            break;
        }
    }
}

void SsMapGraphicsView::setRoute(const QVector<QGeoCoordinate>& route)
{
    for (auto& layerPair : m_layers) {
        if (auto* routeLayer = qobject_cast<RouteLayer*>(layerPair.first)) {
            routeLayer->setRoute(route);
            break;
        }
    }
}

void SsMapGraphicsView::clearRoute()
{
    for (auto& layerPair : m_layers) {
        if (auto* routeLayer = qobject_cast<RouteLayer*>(layerPair.first)) {
            routeLayer->clearRoute();
            break;
        }
    }
}

// 坐标转换方法
QPointF SsMapGraphicsView::geoToScene(const QGeoCoordinate& geo) const
{
    // 简化的墨卡托投影转换
    double x = (geo.longitude() + 180.0) / 360.0 * (1 << int(m_zoomLevel)) * m_tileSize;
    double latRad = qDegreesToRadians(geo.latitude());
    double y = (1.0 - qLn(qTan(latRad) + 1.0 / qCos(latRad)) / M_PI) / 2.0 * (1 << int(m_zoomLevel)) * m_tileSize;
    return QPointF(x, y);
}

QGeoCoordinate SsMapGraphicsView::sceneToGeo(const QPointF& scenePos) const
{
    // 简化的逆墨卡托投影转换
    double lon = scenePos.x() / ((1 << int(m_zoomLevel)) * m_tileSize) * 360.0 - 180.0;
    double n = M_PI - 2.0 * M_PI * scenePos.y() / ((1 << int(m_zoomLevel)) * m_tileSize);
    double lat = qRadiansToDegrees(qAtan(0.5 * (qExp(n) - qExp(-n))));
    return QGeoCoordinate(lat, lon);
}

void SsMapGraphicsView::sceneToTile(const QPointF& scenePos, int& x, int& y) const
{
    x = static_cast<int>(scenePos.x() / m_tileSize);
    y = static_cast<int>(scenePos.y() / m_tileSize);
}

// 瓦片管理
void SsMapGraphicsView::loadVisibleTiles()
{
    if (!viewport()) return;
    
    QRectF viewRect = mapToScene(viewport()->rect()).boundingRect();
    int z = static_cast<int>(floor(m_zoomLevel));
    
    // 计算可见区域的瓦片范围
    int minX, minY, maxX, maxY;
    sceneToTile(viewRect.topLeft(), minX, minY);
    sceneToTile(viewRect.bottomRight(), maxX, maxY);
    
    // 加载可见瓦片
    for (int x = minX; x <= maxX; ++x) {
        for (int y = minY; y <= maxY; ++y) {
            QString key = QString("%1-%2-%3").arg(x).arg(y).arg(z);
            
            if (!m_tileItems.contains(key)) {
                if (m_memoryCache->contains(x, y, z)) {
                    QPixmap tile = m_memoryCache->get(x, y, z);
                    MapTileItem* item = new MapTileItem(x, y, z, tile);
                    m_scene->addItem(item);
                    m_tileItems.insert(key, item);
                } else {
                    m_tileLoader->requestTile(x, y, z);
                }
            }
        }
    }
}

void SsMapGraphicsView::clearAllTiles()
{
    qDeleteAll(m_tileItems);
    m_tileItems.clear();
}

MapTileItem* SsMapGraphicsView::getTileItem(int x, int y, int z)
{
    QString key = QString("%1-%2-%3").arg(x).arg(y).arg(z);
    return m_tileItems.value(key, nullptr);
}

// 事件处理
void SsMapGraphicsView::wheelEvent(QWheelEvent* event)
{
    // 基于鼠标位置的缩放
    QPointF scenePos = mapToScene(event->position().toPoint());
    QGeoCoordinate geoPos = sceneToGeo(scenePos);
    
    double zoomDelta = event->angleDelta().y() > 0 ? 0.5 : -0.5;
    setZoom(m_zoomLevel + zoomDelta);
    
    // 保持鼠标位置的地理坐标不变
    QPointF newScenePos = geoToScene(geoPos);
    centerOn(mapToScene(viewport()->rect().center()) - (newScenePos - scenePos));
    
    event->accept();
}

void SsMapGraphicsView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_lastMousePos = event->pos();
        m_isDragging = true;
        setCursor(Qt::ClosedHandCursor);
    }
    QGraphicsView::mousePressEvent(event);
}

void SsMapGraphicsView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_isDragging) {
        QPoint delta = event->pos() - m_lastMousePos;
        m_lastMousePos = event->pos();
        
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        
        // 更新中心点坐标
        QPointF centerScene = mapToScene(viewport()->rect().center());
        m_center = sceneToGeo(centerScene);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void SsMapGraphicsView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_isDragging = false;
        setCursor(Qt::ArrowCursor);
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void SsMapGraphicsView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    updateMapTiles();
}

void SsMapGraphicsView::showEvent(QShowEvent* event)
{
    QGraphicsView::showEvent(event);
    updateMapTiles();
}

// 槽函数
void SsMapGraphicsView::handleTileReceived(int x, int y, int z, const QPixmap& tile)
{
    m_memoryCache->insert(x, y, z, tile);
    m_diskCache->saveTile(x, y, z, tile);
    
    if (z == static_cast<int>(floor(m_zoomLevel))) {
        QString key = QString("%1-%2-%3").arg(x).arg(y).arg(z);
        if (!m_tileItems.contains(key)) {
            MapTileItem* item = new MapTileItem(x, y, z, tile);
            m_scene->addItem(item);
            m_tileItems.insert(key, item);
        }
    }
}

void SsMapGraphicsView::updateSceneRect(const QRectF& rect)
{
    if (m_scene) {
        m_scene->setSceneRect(rect);
    }
}

void SsMapGraphicsView::updateMapTiles()
{
    // 更新场景矩形
    int tileCount = 1 << static_cast<int>(floor(m_zoomLevel));
    qreal sceneSize = tileCount * m_tileSize;
    updateSceneRect(QRectF(0, 0, sceneSize, sceneSize));

    loadVisibleTiles();
    
    // 更新所有图层
    for (auto& layerPair : m_layers) {
        if (layerPair.second) {
            layerPair.second->update();
        }
    }
}