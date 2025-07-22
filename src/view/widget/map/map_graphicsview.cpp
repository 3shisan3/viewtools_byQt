#include "map_graphicsview.h"

#include <QGraphicsPixmapItem>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStandardPaths>
#include <QtMath>
#include <QWheelEvent>

/**
 * @brief 构造函数，初始化地图视图
 */
SsMapGraphicsView::SsMapGraphicsView(QWidget *parent)
    : QGraphicsView(parent),
      m_scene(new QGraphicsScene(this)),
      m_tileLoader(new SsOnlineTileLoader(this)),
      m_diskCache(QCoreApplication::applicationDirPath() + "/map_tiles")
{
    // 基础设置
    setScene(m_scene);
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // 默认算法
    setTileAlgorithm(TileForCoord::TileAlgorithmFactory::AlgorithmType::Standard);

    // 连接信号槽
    connect(m_tileLoader, &SsOnlineTileLoader::tileReceived, this, &SsMapGraphicsView::handleTileReceived);
    connect(m_tileLoader, &SsOnlineTileLoader::tileFailed, this, &SsMapGraphicsView::handleTileFailed);
}

SsMapGraphicsView::~SsMapGraphicsView()
{
    clearLayers();
}

/**
 * @brief 添加图层到地图
 */
void SsMapGraphicsView::addLayer(BaseLayer *layer)
{
    if (!m_layers.contains(layer))
    {
        m_layers.append(layer);
        connect(layer, &BaseLayer::updateRequested, this, &SsMapGraphicsView::handleLayerUpdateRequested);
        update();
    }
}

/**
 * @brief 从地图移除图层
 */
void SsMapGraphicsView::removeLayer(BaseLayer *layer)
{
    if (m_layers.removeOne(layer))
    {
        disconnect(layer, &BaseLayer::updateRequested, this, &SsMapGraphicsView::handleLayerUpdateRequested);
        update();
    }
}

/**
 * @brief 清空所有图层
 */
void SsMapGraphicsView::clearLayers()
{
    for (auto layer : m_layers)
    {
        disconnect(layer, &BaseLayer::updateRequested, this, &SsMapGraphicsView::handleLayerUpdateRequested);
        layer->deleteLater();
    }
    m_layers.clear();
    update();
}

/**
 * @brief 设置地图中心点
 */
void SsMapGraphicsView::setCenter(const QGeoCoordinate &center)
{
    m_center = center;
    updateViewport();
}

/**
 * @brief 设置缩放级别
 */
void SsMapGraphicsView::setZoomLevel(double zoom)
{
    m_zoomLevel = qBound(1.0, zoom, 22.0);
    updateViewport();
}

/**
 * @brief 平移到指定位置
 */
void SsMapGraphicsView::panTo(const QGeoCoordinate &center)
{
    setCenter(center);
}

/**
 * @brief 缩放到指定级别并平移到指定位置
 */
void SsMapGraphicsView::zoomTo(const QGeoCoordinate &center, double zoom)
{
    m_center = center;
    setZoomLevel(zoom);
}

/**
 * @brief 设置瓦片坐标算法类型
 */
void SsMapGraphicsView::setTileAlgorithm(TileForCoord::TileAlgorithmFactory::AlgorithmType type)
{
    m_tileAlgorithm = TileForCoord::TileAlgorithmFactory::create(type);
    updateViewport();
}

/**
 * @brief 设置瓦片URL模板
 */
void SsMapGraphicsView::setTileUrlTemplate(const QString &urlTemplate, const QStringList &subdomains)
{
    m_tileLoader->setUrlTemplate(urlTemplate, subdomains);
    updateViewport();
}

/**
 * @brief 鼠标滚轮事件处理，实现地图缩放
 */
void SsMapGraphicsView::wheelEvent(QWheelEvent *event)
{
    // 计算缩放中心点
    QPointF scenePos = mapToScene(event->position().toPoint());
    QGeoCoordinate geoPos = pixelToGeo(scenePos);

    // 计算缩放级别
    double zoomDelta = event->angleDelta().y() > 0 ? 1 : -1;
    double newZoom = qBound(1.0, m_zoomLevel + zoomDelta * 0.5, 22.0);

    if (qFuzzyCompare(newZoom, m_zoomLevel))
    {
        return;
    }

    // 保持鼠标位置的地图位置不变
    m_zoomLevel = newZoom;
    m_center = geoPos;

    updateViewport();
    event->accept();
}

/**
 * @brief 鼠标按下事件处理
 */
void SsMapGraphicsView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_lastMousePos = event->pos();
        m_isDragging = true;
        setCursor(Qt::ClosedHandCursor);
    }
    QGraphicsView::mousePressEvent(event);
}

/**
 * @brief 鼠标移动事件处理，实现地图拖动
 */
void SsMapGraphicsView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_isDragging)
    {
        QPointF delta = event->pos() - m_lastMousePos;
        m_lastMousePos = event->pos();

        // 转换为地理坐标偏移
        QPointF centerPixel = geoToPixel(m_center);
        centerPixel -= delta;
        m_center = pixelToGeo(centerPixel);

        updateViewport();
    }
    QGraphicsView::mouseMoveEvent(event);
}

/**
 * @brief 鼠标释放事件处理
 */
void SsMapGraphicsView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_isDragging = false;
        setCursor(Qt::ArrowCursor);
    }
    QGraphicsView::mouseReleaseEvent(event);
}

/**
 * @brief 窗口大小变化事件处理
 */
void SsMapGraphicsView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    updateViewport();
}

/**
 * @brief 绘制事件处理，先绘制瓦片底图，再绘制各图层
 */
void SsMapGraphicsView::paintEvent(QPaintEvent *event)
{
    // 先绘制瓦片底图
    QGraphicsView::paintEvent(event);

    // 然后绘制各图层
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderLayers(&painter);
}

/**
 * @brief 更新视图范围
 */
void SsMapGraphicsView::updateViewport()
{
    // 更新场景矩形
    QPointF centerPixel = geoToPixel(m_center);
    QRectF viewRect(centerPixel - QPointF(width() / 2, height() / 2),
                    QSizeF(width(), height()));
    m_scene->setSceneRect(viewRect);

    // 请求可见区域的瓦片
    requestVisibleTiles();

    // 更新视图
    update();
}

/**
 * @brief 请求当前可见区域的瓦片
 */
void SsMapGraphicsView::requestVisibleTiles()
{
    // 获取当前视图的瓦片范围
    QRectF viewRect = m_scene->sceneRect();
    QPoint topLeftTile = m_tileAlgorithm.latLongToTileXY(
        pixelToGeo(viewRect.topLeft()).longitude(),
        pixelToGeo(viewRect.topLeft()).latitude(),
        qFloor(m_zoomLevel));

    QPoint bottomRightTile = m_tileAlgorithm.latLongToTileXY(
        pixelToGeo(viewRect.bottomRight()).longitude(),
        pixelToGeo(viewRect.bottomRight()).latitude(),
        qFloor(m_zoomLevel));

    // 请求可见的瓦片
    for (int x = topLeftTile.x(); x <= bottomRightTile.x(); ++x)
    {
        for (int y = topLeftTile.y(); y <= bottomRightTile.y(); ++y)
        {
            loadTile(x, y, qFloor(m_zoomLevel));
        }
    }
}

/**
 * @brief 地理坐标转像素坐标
 */
QPointF SsMapGraphicsView::geoToPixel(const QGeoCoordinate &coord) const
{
    QPoint pixel = m_tileAlgorithm.latLongToPixelXY(
        coord.longitude(), coord.latitude(), qFloor(m_zoomLevel));

    // 转换为场景坐标
    QPointF sceneCenter = m_scene->sceneRect().center();
    QPointF viewCenter(width() / 2, height() / 2);

    return QPointF(sceneCenter.x() + (pixel.x() - viewCenter.x()),
                   sceneCenter.y() + (pixel.y() - viewCenter.y()));
}

/**
 * @brief 像素坐标转地理坐标
 */
QGeoCoordinate SsMapGraphicsView::pixelToGeo(const QPointF &pixel) const
{
    // 转换为像素坐标
    QPointF sceneCenter = m_scene->sceneRect().center();
    QPointF viewCenter(width() / 2, height() / 2);
    QPoint pixelPos(pixel.x() - sceneCenter.x() + viewCenter.x(),
                    pixel.y() - sceneCenter.y() + viewCenter.y());

    qreal lon, lat;
    m_tileAlgorithm.pixelXYToLatLong(pixelPos, qFloor(m_zoomLevel), lon, lat);
    return QGeoCoordinate(lat, lon);
}

/**
 * @brief 加载瓦片（优先从缓存，其次从网络）
 */
void SsMapGraphicsView::loadTile(int x, int y, int z)
{
    QString tileKey = QString("%1-%2-%3").arg(x).arg(y).arg(z);

    // 如果已经请求过或正在请求，则跳过
    if (m_requestedTiles.contains(tileKey))
    {
        return;
    }

    // 检查内存缓存
    if (m_memoryCache.contains(x, y, z))
    {
        QPixmap tile = m_memoryCache.get(x, y, z);
        if (!tile.isNull())
        {
            // 添加到场景
            QGraphicsPixmapItem *item = m_scene->addPixmap(tile);
            item->setPos(x * 256, y * 256);
            return;
        }
    }

    // 检查磁盘缓存
    if (m_diskCache.hasTile(x, y, z))
    {
        QPixmap tile = m_diskCache.loadTile(x, y, z);
        if (!tile.isNull())
        {
            // 添加到内存缓存和场景
            m_memoryCache.insert(x, y, z, tile);
            QGraphicsPixmapItem *item = m_scene->addPixmap(tile);
            item->setPos(x * 256, y * 256);
            return;
        }
    }

    // 从网络加载
    m_requestedTiles.insert(tileKey);
    m_tileLoader->requestTile(x, y, z);
}

/**
 * @brief 处理瓦片加载完成
 */
void SsMapGraphicsView::handleTileReceived(int x, int y, int z, const QPixmap &tile)
{
    QString tileKey = QString("%1-%2-%3").arg(x).arg(y).arg(z);
    m_requestedTiles.remove(tileKey);

    if (tile.isNull())
    {
        return;
    }

    // 缓存瓦片
    m_memoryCache.insert(x, y, z, tile);
    m_diskCache.saveTile(x, y, z, tile);

    // 添加到场景
    QGraphicsPixmapItem *item = m_scene->addPixmap(tile);
    item->setPos(x * 256, y * 256);
}

/**
 * @brief 处理瓦片加载失败
 */
void SsMapGraphicsView::handleTileFailed(int x, int y, int z, const QString &error)
{
    qWarning() << "Failed to load tile:" << x << y << z << error;
    QString tileKey = QString("%1-%2-%3").arg(x).arg(y).arg(z);
    m_requestedTiles.remove(tileKey);
}

/**
 * @brief 处理图层更新请求
 */
void SsMapGraphicsView::handleLayerUpdateRequested()
{
    update();
}

/**
 * @brief 渲染所有图层
 */
void SsMapGraphicsView::renderLayers(QPainter *painter)
{
    // 保存当前视图变换
    painter->save();

    // 设置视图变换，使图层渲染与地图坐标对齐
    QTransform transform;
    QPoint centerPixel = m_tileAlgorithm.latLongToPixelXY(
        m_center.longitude(), m_center.latitude(), qFloor(m_zoomLevel));
    transform.translate(width() / 2 - centerPixel.x(), height() / 2 - centerPixel.y());
    painter->setTransform(transform);

    // 按顺序渲染各图层
    for (BaseLayer *layer : m_layers)
    {
        if (layer->isVisible())
        {
            layer->render(painter, viewport()->size(), m_center, m_zoomLevel, m_tileAlgorithm);
        }
    }

    // 恢复视图变换
    painter->restore();
}