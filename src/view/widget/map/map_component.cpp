#include "map_component.h"

#include <QPainter>
#include <QWheelEvent>
#include <QStandardPaths>

MarineMapComponent::MarineMapComponent(QWidget *parent) : QWidget(parent)
{
    // 初始化缓存系统
    QString cachePath = QCoreApplication::applicationDirPath() + "/cache/marine_tiles";
    m_diskCache = new SsDiskCacheManager(cachePath, this);
    m_memoryCache = new SsMemoryCache(100); // 100MB内存缓存

    // 初始化瓦片加载器 (使用高德海洋地图)
    m_tileLoader = new SsOnlineTileLoader(this);
    // https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png
    // m_tileLoader->setUrlTemplate("https://webst02.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}");
    // m_tileLoader->setUrlTemplate("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png");
    m_tileLoader->setUrlTemplate(
        "https://api.mapbox.com/styles/v1/mapbox/streets-v12/tiles/{z}/{x}/{y}?access_token="
        "pk.eyJ1IjoiM3NoaXNhbjMiLCJhIjoiY20wMHRhbnQzMHFmejJtb29ibzk5dzNjaCJ9.CXBCh481VYmC6PPT7jJGSA"
    );
    connect(m_tileLoader, &SsOnlineTileLoader::tileReceived, this, &MarineMapComponent::handleTileReceived);

    // 初始化渲染器
    m_renderer = new MapRenderer(this);

    // 创建图层系统
    m_shipLayer = new ShipLayer(this);
    m_routeLayer = new RouteLayer(this);

    // 初始位置设置为图片中的坐标
    setCenter(QGeoCoordinate(21.48341372, 109.05621073));
    setZoom(12);
}

void MarineMapComponent::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 1. 绘制地图底图
    int tileSize = 256;
    int topTileX = floor((width() / 2 - m_renderer->geoToPixel(m_renderer->center()).x()) / tileSize);
    int topTileY = floor((height() / 2 - m_renderer->geoToPixel(m_renderer->center()).y()) / tileSize);
    int tileCountX = ceil(width() / (float)tileSize) + 1;
    int tileCountY = ceil(height() / (float)tileSize) + 1;

    for (int x = 0; x < tileCountX; ++x)
    {
        for (int y = 0; y < tileCountY; ++y)
        {
            int tileX = topTileX + x;
            int tileY = topTileY + y;

            if (m_memoryCache->contains(tileX, tileY, m_renderer->zoomLevel()))
            {
                QPixmap tile = m_memoryCache->get(tileX, tileY, m_renderer->zoomLevel());
                QPointF pos(x * tileSize - width() / 2 + m_renderer->geoToPixel(m_renderer->center()).x(),
                            y * tileSize - height() / 2 + m_renderer->geoToPixel(m_renderer->center()).y());
                painter.drawPixmap(pos, tile);
            }
            else
            {
                m_tileLoader->requestTile(tileX, tileY, m_renderer->zoomLevel());
            }
        }
    }

    // 2. 绘制图层
    m_routeLayer->render(&painter, size(), m_renderer->center(), m_renderer->zoomLevel());
    m_shipLayer->render(&painter, size(), m_renderer->center(), m_renderer->zoomLevel());
}

void MarineMapComponent::handleTileReceived(int x, int y, int z, const QPixmap &tile)
{
    m_memoryCache->insert(x, y, z, tile);
    m_diskCache->saveTile(x, y, z, tile);
    update();
}

void MarineMapComponent::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}

void MarineMapComponent::wheelEvent(QWheelEvent *event)
{
    QPoint numDegrees = event->angleDelta() / 8;
    if (!numDegrees.isNull())
    {
        double zoomDelta = numDegrees.y() > 0 ? 1 : -1;
        setZoom(m_renderer->zoomLevel() + zoomDelta);
    }
    event->accept();
}

void MarineMapComponent::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_lastMousePos = event->pos();
        m_isDragging = true;
        setCursor(Qt::ClosedHandCursor);
    }
    QWidget::mousePressEvent(event);
}

void MarineMapComponent::mouseMoveEvent(QMouseEvent *event)
{
    if (m_isDragging)
    {
        QPoint delta = event->pos() - m_lastMousePos;
        m_lastMousePos = event->pos();

        QPointF centerPixel = m_renderer->geoToPixel(m_renderer->center());
        centerPixel -= delta;
        setCenter(m_renderer->pixelToGeo(centerPixel));
    }
    QWidget::mouseMoveEvent(event);
}

void MarineMapComponent::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_isDragging = false;
        setCursor(Qt::ArrowCursor);
    }
    QWidget::mouseReleaseEvent(event);
}

void MarineMapComponent::setCenter(const QGeoCoordinate &center)
{
    m_renderer->setCenter(center);
    update();
}

void MarineMapComponent::setZoom(double zoom)
{
    // 限制缩放级别在合理范围内
    zoom = qBound(3.0, zoom, 18.0);
    m_renderer->setZoomLevel(zoom);
    update();
}

void MarineMapComponent::setMapStyle(const QString &styleUrl)
{
    m_tileLoader->setUrlTemplate(styleUrl);
    // 清除缓存以加载新样式
    m_memoryCache->clear();
    m_diskCache->clearCache();
    update();
}

void MarineMapComponent::updateShipData(const QGeoCoordinate &position, double heading)
{
    m_shipLayer->setShipPosition(position, heading);
    update();
}

void MarineMapComponent::setRoute(const QVector<QGeoCoordinate> &route)
{
    m_routeLayer->setRoute(route);
    update();
}

void MarineMapComponent::clearRoute()
{
    m_routeLayer->clearRoute();
    update();
}