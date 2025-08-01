#include "map_graphicsview.h"

#include <QGraphicsPixmapItem>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStandardPaths>
#include <QtMath>
#include <QWheelEvent>

#if QT_VERSION_MAJOR >= 6
    using TouchDeviceType = QPointingDevice;
#else
    using TouchDeviceType = QTouchDevice;
#endif

/**
 * @brief 构造函数，初始化地图视图
 */
SsMapGraphicsView::SsMapGraphicsView(QWidget *parent)
    : QGraphicsView(parent)
    , m_scene(new QGraphicsScene(this))
    , m_tileLoader(new SsOnlineTileLoader())  // 无父对象
    , m_diskCache(QCoreApplication::applicationDirPath() + "/map_tiles")
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
    if (TouchDeviceType::devices().count() > 0) // 判断是否为触控设备
    {
        setAttribute(Qt::WA_AcceptTouchEvents); // 启用触摸事件
        grabGesture(Qt::PinchGesture);          // 捏合手势
        grabGesture(Qt::PanGesture);            // 平移手势
    }

    // 默认配置
    setZoomBehavior(true); // 开启鼠标追踪(如果想要放大缩小鼠标所在位置（以视图中心放大缩小可注释）类似功能需要启用)
    setTileSaveDisk(false); // 默认开启自动缓存瓦片到磁盘
    // 默认算法
    setTileAlgorithm(TileForCoord::TileAlgorithmFactory::AlgorithmType::Standard);

    // 启动瓦片加载线程
    m_tileLoader->start();
    // 连接信号槽(自动跨线程工作)
    connect(m_tileLoader, &SsOnlineTileLoader::tileReceived, this, &SsMapGraphicsView::handleTileReceived);
    connect(m_tileLoader, &SsOnlineTileLoader::tileFailed, this, &SsMapGraphicsView::handleTileFailed);
}

SsMapGraphicsView::~SsMapGraphicsView()
{
    clearLayers();

    // 停止瓦片加载线程
    if(m_tileLoader)
    {
        m_tileLoader->stop();
        delete m_tileLoader;  // 手动释放
    }
}

/**
 * @brief 获取当前视图中心点对应的地理坐标
 */
QGeoCoordinate SsMapGraphicsView::currentCenter() const
{
    QPointF viewCenter(viewport()->width() / 2.0, viewport()->height() / 2.0);
    return pixelToGeo(mapToScene(viewCenter.toPoint()));
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
 * @brief 设置瓦片地图是否自动缓存到磁盘，及缓存父目录
 */
void SsMapGraphicsView::setTileSaveDisk(bool toAutoSave, const QString &saveDir)
{
    m_autoSaveDisk = toAutoSave;

    m_memoryCache.clear();  // 两点，1.简化处理在内存中瓦片无法存到磁盘问题；2.使用本地瓦片除去内存残留瓦片干扰
    if (m_autoSaveDisk && !saveDir.isEmpty())
    {
        m_diskCache.setSaveDir(saveDir);
    }
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
 * @brief 设置地图中心点(平移到指定位置)
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
    m_memoryCache.clear();
    updateViewport();
    update();
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
    }

    setCenter(currentCenter());
    event->accept();
}

/**
 * @brief 鼠标按下事件处理
 */
void SsMapGraphicsView::mousePressEvent(QMouseEvent *event)
{
    if (m_activeTouchId != -1) return; // 触控优先
    if (event->source() != Qt::MouseEventNotSynthesized) return;

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
    QGraphicsView::mouseMoveEvent(event);
    if (m_isDragging)
    {
        setCenter(currentCenter());
    }
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

        // 使用 setCenter 来更新中心点，复用相同的逻辑
        setCenter(currentCenter());
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
 * @brief 窗口显示时设置显示瓦片的视图位置
 */
void SsMapGraphicsView::showEvent(QShowEvent* event)
{
    QGraphicsView::showEvent(event);
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
 * @brief 重写event函数拦截手势事件
 */
bool SsMapGraphicsView::event(QEvent *event)
{
    switch (event->type())
    {
    case QEvent::Gesture:
        gestureEvent(static_cast<QGestureEvent *>(event));
        return true;
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    {
        QTouchEvent *touchEvent = static_cast<QTouchEvent *>(event);
#if QT_VERSION_MAJOR >= 6
        const auto &touchPoints = touchEvent->points();
#else
        const auto &touchPoints = touchEvent->touchPoints();
#endif
        if (m_activeTouchId != -1)
        {
            for (const auto &point : touchPoints)
            {
                if (point.id() == m_activeTouchId)
                {
#if QT_VERSION_MAJOR >= 6
                    handleTouchPoint(point.position(), event->type() == QEvent::TouchEnd);
#else
                    handleTouchPoint(point.pos(), event->type() == QEvent::TouchEnd);
#endif
                    if (event->type() == QEvent::TouchEnd)
                    {
                        m_activeTouchId = -1;
                    }
                    return true;
                }
            }
            m_activeTouchId = -1;
        }
        if (event->type() == QEvent::TouchBegin && !touchPoints.isEmpty())
        {
#if QT_VERSION_MAJOR >= 6
            m_activeTouchId = touchPoints.first().id();
            handleTouchPoint(touchPoints.first().position());
#else
            m_activeTouchId = touchPoints.first().id();
            handleTouchPoint(touchPoints.first().pos());
#endif
        }
        return true;
    }
    default:
        return QGraphicsView::event(event);
    }
}

// 手势处理
void SsMapGraphicsView::gestureEvent(QGestureEvent *event)
{
    if (QGesture *pinch = event->gesture(Qt::PinchGesture))
    {
        handlePinchGesture(static_cast<QPinchGesture *>(pinch));
    }
    event->accept();
}

void SsMapGraphicsView::handlePinchGesture(QPinchGesture *gesture)
{
    static qreal initialZoom = 0;
    switch (gesture->state())
    {
    case Qt::GestureStarted:
        initialZoom = m_zoomLevel;
        break;
    case Qt::GestureUpdated:
        setZoomLevel(initialZoom * gesture->scaleFactor());
        break;
    case Qt::GestureFinished:
        break;
    default:
        break;
    }
}

// 触控点处理
void SsMapGraphicsView::handleTouchPoint(const QPointF &pos, bool isRelease)
{
    if (isRelease)
    {
        m_isDragging = false;
        return;
    }

    if (!m_isDragging)
    {
        m_lastMousePos = pos.toPoint();
        m_isDragging = true;
    }
    else
    {
        QPoint delta = pos.toPoint() - m_lastMousePos;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        m_lastMousePos = pos.toPoint();
    }
}

/**
 * @brief 更新视图范围（核心方法）
 */
void SsMapGraphicsView::updateViewport()
{
    // 确保场景大小正确
    int w = int(qPow(2, m_zoomLevel) * 256);
    m_scene->setSceneRect(0, 0, w, w);

    // 计算中心点对应的像素坐标
    QPointF pixel = m_tileAlgorithm.latLongToPixelXY(
        m_center.longitude(), m_center.latitude(), qFloor(m_zoomLevel));
    
    // 计算视图中心应该对准的scene位置
    QPointF sceneCenter(pixel.x() - width()/2, pixel.y() - height()/2);
    
    // 直接设置滚动条位置（避免centerOn的潜在问题）
    horizontalScrollBar()->setValue(sceneCenter.x());
    verticalScrollBar()->setValue(sceneCenter.y());

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

    // 直接使用像素坐标计算瓦片范围
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
    return m_tileAlgorithm.latLongToPixelXY(
        coord.longitude(), coord.latitude(), qFloor(m_zoomLevel));
}

/**
 * @brief 像素坐标转地理坐标
 */
QGeoCoordinate SsMapGraphicsView::pixelToGeo(const QPointF &pixel) const
{
    double lon, lat;
    // 直接调用算法，无需额外偏移计算
    m_tileAlgorithm.pixelXYToLatLong(
        pixel,
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
    if (m_autoSaveDisk && m_diskCache.hasTile(x, y, z))
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
    QMetaObject::invokeMethod(m_tileLoader, [this, x, y, z]() {
        m_tileLoader->requestTile(x, y, z);
    }, Qt::QueuedConnection);
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
    if (m_autoSaveDisk)
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