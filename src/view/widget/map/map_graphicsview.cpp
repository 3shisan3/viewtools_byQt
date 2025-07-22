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
    setScene(m_scene);  // QGraphicsView 默认 paintevent 绘制 QGraphicsScene 的内容
    // 绘制参数
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::ScrollHandDrag); // 启用拖拽
    // 隐藏滚动条
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // 默认配置
    setZoomBehavior(true); // 开启鼠标追踪(如果想要放大缩小鼠标所在位置（以视图中心放大缩小可注释）类似功能需要启用)
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
 * @brief 设置地图缩放基于地图中心还是鼠标所在位置
 */
void SsMapGraphicsView::setZoomBehavior(bool zoomAtMousePosition)
{
    m_zoomAtMousePos = zoomAtMousePosition;
    setMouseTracking(zoomAtMousePosition);
};

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
    if (m_center != center)
    {
        m_center = center;

        // 更新视图
        updateViewport();
    }
}

/**
 * @brief 设置缩放级别
 */
void SsMapGraphicsView::setZoomLevel(double zoom)
{
    if (m_zoomLevel != zoom)
    {
        m_zoomLevel = qBound(1.0, zoom, 22.0);
        updateViewport();
    }
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
    // 计算缩放级别变化
    double zoomDelta = event->angleDelta().y() > 0 ? 1 : -1;
    double newZoom = qBound(1.0, m_zoomLevel + zoomDelta, 22.0);

    if (qFuzzyCompare(newZoom, m_zoomLevel))
    {
        event->accept();
        return;
    }

    // 计算缩放比例因子
    double scaleFactor = qPow(2.0, newZoom - m_zoomLevel);
    // 更新缩放级别
    m_zoomLevel = newZoom;

    // ================= 基于鼠标位置缩放 =================
    if (m_zoomAtMousePos)
    {
        // 保存鼠标位置（视图坐标和场景坐标）
        QPoint mousePos = event->position().toPoint();
        QPointF mouseScenePos = mapToScene(mousePos);
        
        // 更新视图范围
        int w = int(qPow(2, m_zoomLevel) * 256);
        m_scene->setSceneRect(0, 0, w, w);

        // 计算鼠标点在缩放前后的位置变化
        QPointF newMouseScenePos = mouseScenePos * scaleFactor;
        
        // 通过滚动条调整视图位置（保持鼠标点视觉位置不变）
        horizontalScrollBar()->setValue(qRound(newMouseScenePos.x() - mousePos.x()));
        verticalScrollBar()->setValue(qRound(newMouseScenePos.y() - mousePos.y()));

        // 同步 m_center 到当前视图中心
        QPointF viewCenter = mapToScene(viewport()->rect().center());
        m_center = pixelToGeo(viewCenter);  // todo存在公式导致的误差，待解决
    }

    // 统一请求瓦片
    requestVisibleTiles();
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
        QGeoCoordinate newCenter = pixelToGeo(centerPixel);

        // 使用 setCenter 来更新中心点，复用相同的逻辑
        setCenter(newCenter);
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
 * @brief       窗口显示时设置显示瓦片的视图位置
 * @param event
 */
void SsMapGraphicsView::showEvent(QShowEvent* event)
{
    QGraphicsView::showEvent(event);
    
    int w = int(qPow(2, m_zoomLevel) * 256);
    m_scene->setSceneRect(0, 0, w, w);
    
    // 初始化视图位置
    QPoint pixel = m_tileAlgorithm.latLongToPixelXY(
        m_center.longitude(), m_center.latitude(), qFloor(m_zoomLevel));
    QPointF scenePos(pixel.x() - width()/2, pixel.y() - height()/2);
    centerOn(scenePos);
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
    // 确保场景大小正确
    int w = int(qPow(2, m_zoomLevel) * 256);
    m_scene->setSceneRect(0, 0, w, w);

    // 重新计算并设置中心点
    QPoint pixel = m_tileAlgorithm.latLongToPixelXY(
        m_center.longitude(), m_center.latitude(), qFloor(m_zoomLevel));
    centerOn(QPointF(pixel.x() - width()/2, pixel.y() - height()/2));

    // 请求可见区域的瓦片
    requestVisibleTiles();
}

/**
 * @brief 请求当前可见区域的瓦片
 */
void SsMapGraphicsView::requestVisibleTiles()
{
    // 获取当前视图的瓦片范围
    QRectF viewRect = mapToScene(viewport()->rect()).boundingRect();

    // 直接使用像素坐标计算瓦片范围（更高效）
    QPoint tlTile = QPoint(qFloor(viewRect.left() / 256.0),
                           qFloor(viewRect.top() / 256.0));
    QPoint brTile = QPoint(qFloor(viewRect.right() / 256.0),
                           qFloor(viewRect.bottom() / 256.0));

    // 添加边界检查
    int maxTile = qPow(2, m_zoomLevel);
    tlTile.setX(qMax(0, tlTile.x()));
    tlTile.setY(qMax(0, tlTile.y()));
    brTile.setX(qMin(maxTile - 1, brTile.x()));
    brTile.setY(qMin(maxTile - 1, brTile.y()));

    // 请求瓦片
    for (int x = tlTile.x(); x <= brTile.x(); ++x)
    {
        for (int y = tlTile.y(); y <= brTile.y(); ++y)
        {
            loadTile(x, y, m_zoomLevel);
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

    return QPointF(pixel.x(), pixel.y());
}

/**
 * @brief 像素坐标转地理坐标
 */
QGeoCoordinate SsMapGraphicsView::pixelToGeo(const QPointF &pixel) const
{
    qreal lon, lat;
    // 直接调用算法，无需额外偏移计算
    m_tileAlgorithm.pixelXYToLatLong(
        QPoint(static_cast<int>(pixel.x()), static_cast<int>(pixel.y())),
        qFloor(m_zoomLevel),
        lon, lat
    );
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