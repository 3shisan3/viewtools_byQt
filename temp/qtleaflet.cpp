#include "qtleaflet.h"
#include <QGraphicsPixmapItem>
#include <QScrollBar>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <QtMath>
#include <QNetworkReply>
#include <QNetworkDiskCache>
#include <QImage>
#include <QBuffer>
#include <QPainter>

// 常量定义
const double INITIAL_LAT = 39.9042;  // 北京纬度
const double INITIAL_LNG = 116.4074; // 北京经度
const int INITIAL_ZOOM = 12;
const int TILE_SIZE = 256;
const double MIN_LAT = -85.05112878;
const double MAX_LAT = 85.05112878;
const double MIN_LNG = -180.0;
const double MAX_LNG = 180.0;

// TileLoader 实现
TileLoader::TileLoader(QObject *parent) : QObject(parent),
    m_networkManager(new QNetworkAccessManager(this)),
    m_sourceType(RemoteHttp)
{
    // 设置网络缓存
    QNetworkDiskCache *diskCache = new QNetworkDiskCache(this);
    QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/qtleaflet";
    diskCache->setCacheDirectory(cachePath);
    m_networkManager->setCache(diskCache);

    connect(m_networkManager, &QNetworkAccessManager::finished, 
            this, &TileLoader::handleNetworkReply);
}

TileLoader::~TileLoader()
{
    m_networkManager->deleteLater();
}

void TileLoader::setTileSource(const QString &source, TileSourceType type)
{
    m_tileSource = source;
    m_sourceType = type;
    emit debugMessage(QString("Tile source set to: %1 (Type: %2)")
                     .arg(source)
                     .arg(type == RemoteHttp ? "RemoteHttp" : "LocalFile"));
}

QString TileLoader::tileSource() const
{
    return m_tileSource;
}

TileLoader::TileSourceType TileLoader::sourceType() const
{
    return m_sourceType;
}

void TileLoader::requestTile(const QPoint &tilePos, int zoom)
{
    if (m_tileSource.isEmpty()) {
        emit debugMessage("Tile source not set");
        return;
    }
    
    QString urlStr = m_tileSource;
    urlStr.replace("{x}", QString::number(tilePos.x()));
    urlStr.replace("{y}", QString::number(tilePos.y()));
    urlStr.replace("{z}", QString::number(zoom));
    
    if (m_sourceType == RemoteHttp) {
        // 处理远程HTTP请求
        if (urlStr.contains("{s}")) {
            static const QStringList subdomains = {"a", "b", "c"};
            static int subdomainIndex = 0;
            urlStr.replace("{s}", subdomains.at(subdomainIndex++ % subdomains.size()));
        }
        
        QUrl url(urlStr);
        if (!url.isValid()) {
            emit debugMessage("Invalid URL: " + urlStr);
            return;
        }
        
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, 
                           QNetworkRequest::PreferCache);
        request.setRawHeader("User-Agent", "QtLeaflet/1.0");
        
        emit debugMessage(QString("Requesting remote tile: %1").arg(url.toString()));
        
        QNetworkReply *reply = m_networkManager->get(request);
        connect(reply, &QNetworkReply::errorOccurred, [=](QNetworkReply::NetworkError error) {
            emit debugMessage(QString("Network error: %1 - %2").arg(error).arg(reply->errorString()));
        });
        
        m_activeRequests[url] = qMakePair(tilePos, zoom);
    } else {
        // 处理本地文件请求
        QUrl fileUrl(urlStr);
        QString filePath = fileUrl.toLocalFile();
        
        emit debugMessage(QString("Loading local tile: %1").arg(filePath));
        
        if (QFile::exists(filePath)) {
            QPixmap pixmap;
            if (pixmap.load(filePath)) {
                emit tileLoaded(pixmap, tilePos, zoom);
            } else {
                emit debugMessage("Failed to load local tile: " + filePath);
            }
        } else {
            emit debugMessage("Local tile not found: " + filePath);
        }
    }
}

void TileLoader::handleNetworkReply(QNetworkReply *reply)
{
    QUrl url = reply->url();
    if (!m_activeRequests.contains(url)) {
        reply->deleteLater();
        return;
    }
    
    QPair<QPoint, int> tileInfo = m_activeRequests.take(url);
    QPoint tilePos = tileInfo.first;
    int zoom = tileInfo.second;
    
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        QPixmap pixmap;
        if (pixmap.loadFromData(data)) {
            emit tileLoaded(pixmap, tilePos, zoom);
            emit debugMessage(QString("Tile loaded: %1,%2 zoom %3").arg(tilePos.x()).arg(tilePos.y()).arg(zoom));
        } else {
            emit debugMessage("Failed to decode tile image");
        }
    } else {
        emit debugMessage(QString("Failed to load tile: %1").arg(reply->errorString()));
    }
    
    reply->deleteLater();
}

// TileCache 实现
TileCache::TileCache(QObject *parent) : QObject(parent),
    m_memoryCache(100)  // 缓存100个瓦片
{
    m_cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/qtleaflet/tiles";
    QDir().mkpath(m_cachePath);
}

TileCache::~TileCache()
{
    m_memoryCache.clear();
}

void TileCache::setCachePath(const QString &path)
{
    if (m_cachePath != path) {
        m_cachePath = path;
        QDir().mkpath(m_cachePath);
    }
}

QString TileCache::cachePath() const
{
    return m_cachePath;
}

bool TileCache::hasTile(const QString &key) const
{
    if (m_memoryCache.contains(key)) {
        return true;
    }
    
    return QFile::exists(tilePath(key));
}

bool TileCache::getTile(const QString &key, QPixmap *pixmap) const
{
    // 从内存缓存获取
    if (m_memoryCache.contains(key)) {
        *pixmap = *m_memoryCache.object(key);
        return true;
    }
    
    // 从磁盘缓存获取
    if (!m_cachePath.isEmpty()) {
        QString filePath = tilePath(key);
        if (QFile::exists(filePath)) {
            if (pixmap->load(filePath, "PNG")) {
                // 放入内存缓存
                m_memoryCache.insert(key, new QPixmap(*pixmap));
                return true;
            }
        }
    }
    
    return false;
}

void TileCache::putTile(const QString &key, const QPixmap &pixmap)
{
    if (m_cachePath.isEmpty()) return;
    
    // 放入内存缓存
    m_memoryCache.insert(key, new QPixmap(pixmap));
    
    // 保存为PNG文件
    QString filePath = tilePath(key);
    QDir().mkpath(QFileInfo(filePath).path());
    
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        QImage image = pixmap.toImage();
        image.save(&file, "PNG");
        file.close();
    }
}

QString TileCache::tilePath(const QString &key) const
{
    return m_cachePath + "/" + key + ".png";
}

// QtLeaflet 实现
QtLeaflet::QtLeaflet(QWidget *parent) : QGraphicsView(parent),
    m_scene(new QGraphicsScene(this)),
    m_tileLoader(new TileLoader(this)),
    m_tileCache(new TileCache(this)),
    m_centerLat(INITIAL_LAT),
    m_centerLng(INITIAL_LNG),
    m_zoom(INITIAL_ZOOM),
    m_dragging(false)
{
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing);
    setRenderHint(QPainter::SmoothPixmapTransform);
    setDragMode(QGraphicsView::NoDrag);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    
    initScene();
    
    connect(m_tileLoader, &TileLoader::tileLoaded, 
            this, &QtLeaflet::handleTileLoaded);
    connect(m_tileLoader, &TileLoader::debugMessage,
            this, &QtLeaflet::debugMessage);
}

QtLeaflet::~QtLeaflet()
{
    m_tileLoader->deleteLater();
    m_tileCache->deleteLater();
}

void QtLeaflet::initScene()
{
    m_scene->setSceneRect(-10000, -10000, 20000, 20000);
    centerOn(latLngToScenePos(m_centerLat, m_centerLng));
    loadVisibleTiles();
}

void QtLeaflet::setCenter(double lat, double lng)
{
    m_centerLat = qBound(MIN_LAT, lat, MAX_LAT);
    m_centerLng = qBound(MIN_LNG, lng, MAX_LNG);
    centerOn(latLngToScenePos(m_centerLat, m_centerLng));
    loadVisibleTiles();
}

void QtLeaflet::setZoom(int zoom)
{
    if (zoom < 0 || zoom > 20) return;
    
    if (m_zoom != zoom) {
        m_zoom = zoom;
        m_scene->clear();
        m_markers.clear();
        loadVisibleTiles();
    }
}

int QtLeaflet::zoom() const
{
    return m_zoom;
}

void QtLeaflet::setTileSource(TileProvider provider)
{
    switch (provider) {
    case OpenStreetMap:
        m_tileLoader->setTileSource("https://tile.openstreetmap.org/{z}/{x}/{y}.png", 
                                   TileLoader::RemoteHttp);
        break;
    case GoogleMap:
        m_tileLoader->setTileSource("https://mt.google.com/vt/lyrs=m&x={x}&y={y}&z={z}", 
                                   TileLoader::RemoteHttp);
        break;
    case LocalFileSystem:
        m_tileLoader->setTileSource("file:///path/to/tiles/{z}/{x}/{y}.png", 
                                   TileLoader::LocalFile);
        break;
    }
}

void QtLeaflet::setCustomTileSource(const QString &source, TileLoader::TileSourceType type)
{
    m_tileLoader->setTileSource(source, type);
}

QString QtLeaflet::tileSource() const
{
    return m_tileLoader->tileSource();
}

void QtLeaflet::setCacheEnabled(bool enabled)
{
    m_tileCache->setCachePath(enabled ? 
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/qtleaflet/tiles" : 
        "");
}

void QtLeaflet::setCachePath(const QString &path)
{
    m_tileCache->setCachePath(path);
}

QString QtLeaflet::cachePath() const
{
    return m_tileCache->cachePath();
}

void QtLeaflet::addMarker(double lat, double lng, const QPixmap &icon)
{
    QString key = QString("%1_%2").arg(lat).arg(lng);
    if (m_markers.contains(key)) {
        return;
    }
    
    QGraphicsPixmapItem *marker = m_scene->addPixmap(icon);
    marker->setPos(latLngToScenePos(lat, lng));
    marker->setZValue(1);
    marker->setOffset(-icon.width()/2, -icon.height());
    m_markers.insert(key, marker);
}

void QtLeaflet::clearMarkers()
{
    for (QGraphicsPixmapItem *marker : m_markers) {
        m_scene->removeItem(marker);
        delete marker;
    }
    m_markers.clear();
}

void QtLeaflet::wheelEvent(QWheelEvent *event)
{
    int delta = event->angleDelta().y();
    if (delta > 0) {
        setZoom(m_zoom + 1);
    } else if (delta < 0) {
        setZoom(m_zoom - 1);
    }
    event->accept();
}

void QtLeaflet::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_lastMousePos = event->pos();
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
    }
    QGraphicsView::mousePressEvent(event);
}

void QtLeaflet::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        QPoint delta = event->pos() - m_lastMousePos;
        m_lastMousePos = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        loadVisibleTiles();
    }
    QGraphicsView::mouseMoveEvent(event);
}

void QtLeaflet::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
        
        // 更新中心点
        QPointF center = mapToScene(viewport()->rect().center());
        QPointF latLng = scenePosToLatLng(center);
        m_centerLat = latLng.x();
        m_centerLng = latLng.y();
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void QtLeaflet::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    loadVisibleTiles();
}

void QtLeaflet::handleTileLoaded(const QPixmap &pixmap, const QPoint &tilePos, int zoom)
{
    if (zoom != m_zoom) return;
    
    QString key = tileKey(tilePos, zoom);
    m_tileCache->putTile(key, pixmap);
    
    QGraphicsPixmapItem *tile = m_scene->addPixmap(pixmap);
    tile->setPos(tilePosToScenePos(tilePos));
    tile->setZValue(0);
}

void QtLeaflet::updateTiles()
{
    loadVisibleTiles();
}

void QtLeaflet::loadVisibleTiles()
{
    QRectF viewRect = mapToScene(viewport()->rect()).boundingRect();
    
    // 计算可见区域的瓦片范围
    QPointF topLeft = viewRect.topLeft();
    QPointF bottomRight = viewRect.bottomRight();
    
    QPoint tileTopLeft = latLngToTilePos(scenePosToLatLng(topLeft).x(), 
                                        scenePosToLatLng(topLeft).y(), m_zoom);
    QPoint tileBottomRight = latLngToTilePos(scenePosToLatLng(bottomRight).x(), 
                                           scenePosToLatLng(bottomRight).y(), m_zoom);
    
    // 加载可见瓦片
    for (int x = tileTopLeft.x() - 1; x <= tileBottomRight.x() + 1; ++x) {
        for (int y = tileTopLeft.y() - 1; y <= tileBottomRight.y() + 1; ++y) {
            QString key = tileKey(QPoint(x, y), m_zoom);
            
            // 检查缓存
            QPixmap cachedPixmap;
            if (m_tileCache->hasTile(key)) {
                if (m_tileCache->getTile(key, &cachedPixmap)) {
                    QGraphicsPixmapItem *tile = m_scene->addPixmap(cachedPixmap);
                    tile->setPos(tilePosToScenePos(QPoint(x, y)));
                    tile->setZValue(0);
                    continue;
                }
            }
            
            // 请求新瓦片
            m_tileLoader->requestTile(QPoint(x, y), m_zoom);
        }
    }
}

QPointF QtLeaflet::latLngToScenePos(double lat, double lng) const
{
    // 将经纬度转换为场景坐标
    lat = qBound(MIN_LAT, lat, MAX_LAT);
    lng = qBound(MIN_LNG, lng, MAX_LNG);
    
    double x = lng / 360.0 + 0.5;
    double sinLat = qSin(lat * M_PI / 180.0);
    double y = 0.5 - 0.25 * qLn((1 + sinLat) / (1 - sinLat)) / M_PI;
    
    qreal mapSize = qPow(2.0, m_zoom) * TILE_SIZE;
    return QPointF(x * mapSize, y * mapSize);
}

QPointF QtLeaflet::scenePosToLatLng(const QPointF &pos) const
{
    qreal mapSize = qPow(2.0, m_zoom) * TILE_SIZE;
    double x = pos.x() / mapSize - 0.5;
    double y = 0.5 - pos.y() / mapSize;
    
    double lng = x * 360.0;
    double lat = 90.0 - 360.0 * qAtan(qExp(-y * 2 * M_PI)) / M_PI;
    
    return QPointF(lat, lng);
}

QPoint QtLeaflet::latLngToTilePos(double lat, double lng, int zoom) const
{
    QPointF scenePos = latLngToScenePos(lat, lng);
    qreal mapSize = qPow(2.0, zoom) * TILE_SIZE;
    int x = qFloor(scenePos.x() / TILE_SIZE);
    int y = qFloor(scenePos.y() / TILE_SIZE);
    return QPoint(x, y);
}

QPointF QtLeaflet::tilePosToScenePos(const QPoint &tilePos) const
{
    return QPointF(tilePos.x() * TILE_SIZE, tilePos.y() * TILE_SIZE);
}

QString QtLeaflet::tileKey(const QPoint &tilePos, int zoom) const
{
    return QString("%1_%2_%3").arg(zoom).arg(tilePos.x()).arg(tilePos.y());
}