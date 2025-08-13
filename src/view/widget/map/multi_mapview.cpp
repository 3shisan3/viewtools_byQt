#include "multi_mapview.h"

#include <QGraphicsLineItem>
#include <QMenu>

SsMultiMapView::SsMultiMapView(QWidget *parent)
    : SsMapGraphicsView(parent)
    , m_measureState(MeasureNone)
    , m_measurePathItem(nullptr)
    , m_tempLineItem(nullptr)
    , m_tempDistanceText(nullptr)
    , m_routeState(RouteNone)
    , m_tempRouteLineItem(nullptr)
{
    // 初始化路线图层
    m_routeLayer = new RouteLayer(this);
    addLayer(m_routeLayer);
    m_routeLayer->setVisible(false);
}

SsMultiMapView::~SsMultiMapView()
{
    clearMeasurement();
    clearRoute();
}

void SsMultiMapView::setRouteStyle(const QColor &color, int width, Qt::PenStyle style)
{
    if (m_routeLayer)
    {
        m_routeLayer->setLineColor(color);
        m_routeLayer->setLineWidth(width);
        m_routeLayer->setLineStyle(style);
    }
}

// ================= 测距功能 =================
void SsMultiMapView::startDistanceMeasurement()
{
    clearMeasurement();
    m_measureState = MeasureStartPoint;
    setCursor(Qt::CrossCursor);
    emit measurementStarted();
}

void SsMultiMapView::cancelDistanceMeasurement()
{
    clearMeasurement();
    setCursor(Qt::ArrowCursor);
    emit measurementCanceled();
}

// ================= 路线规划功能 =================
void SsMultiMapView::startRoutePlanning()
{
    // 开始规划时，默认退出测量状态
    if (isMeasuring())
        cancelDistanceMeasurement();

    clearRoute();
    m_routeState = RouteStartPoint;
    m_routeLayer->setVisible(true);
    setCursor(Qt::CrossCursor);
    emit routePlanningStarted();
}

void SsMultiMapView::cancelRoutePlanning()
{
    clearRoute();
    m_routeState = RouteStartPoint;
    setCursor(Qt::CrossCursor);
    emit routePlanningCanceled();
}

void SsMultiMapView::finishRoutePlanning()
{
    if (m_routeState != RouteNone && !m_routeLayer->route().isEmpty())
    {
        m_routeState = RouteEndPoint;
        setCursor(Qt::ArrowCursor);
        emit routePlanningFinished(m_routeLayer->route());
    }
}

void SsMultiMapView::exitRoutePlanning()
{
    clearRoute();
    m_routeState = RouteNone;
    m_routeLayer->setVisible(false);
    setCursor(Qt::ArrowCursor);
    emit routePlanningExited();
}

// ================= 事件处理 =================
void SsMultiMapView::mousePressEvent(QMouseEvent *event)
{
    // 处理测距模式下的点击
    if (m_measureState != MeasureNone && event->button() == Qt::LeftButton)
    {
        QGeoCoordinate coord = pixelToGeo(mapToScene(event->pos()));

        if (m_measureState == MeasureStartPoint)
        {
            addMeasurePoint(coord);
            m_measureState = MeasureMiddlePoints;
        }
        else if (m_measureState == MeasureMiddlePoints)
        {
            addMeasurePoint(coord);
        }
    }

    // 处理路线规划模式下的点击
    if (m_routeState != RouteNone && event->button() == Qt::LeftButton &&
        m_measureState == MeasureNone)
    {
        QGeoCoordinate coord = pixelToGeo(mapToScene(event->pos()));

        if (m_routeState == RouteStartPoint)
        {
            m_routeLayer->setRoute({coord});
            m_routeState = RouteMiddlePoints;
        }
        else if (m_routeState == RouteMiddlePoints)
        {
            addRoutePoint(coord);
        }
    }

    SsMapGraphicsView::mousePressEvent(event);
}

void SsMultiMapView::mouseMoveEvent(QMouseEvent *event)
{
    // 在测距模式下更新临时线
    if (m_measureState == MeasureMiddlePoints && !m_measurePoints.isEmpty())
    {
        updateTempLine(pixelToGeo(mapToScene(event->pos())));
    }

    // 在路线规划模式下更新临时线
    if (m_routeState == RouteMiddlePoints && !m_routeLayer->route().isEmpty() &&
        m_measureState == MeasureNone)
    {
        updateTempRouteLine(pixelToGeo(mapToScene(event->pos())));
    }

    SsMapGraphicsView::mouseMoveEvent(event);
}

void SsMultiMapView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // 测距模式下双击结束测距
    if (m_measureState != MeasureNone && event->button() == Qt::LeftButton)
    {
        if (m_measureState == MeasureMiddlePoints && !m_measurePoints.isEmpty())
            endDistanceMeasurement();
        else if (m_measureState == MeasureEndPoint)
            cancelDistanceMeasurement();
        
        event->accept();
        return;
    }
    
    // 路线规划模式下双击结束规划
    if (m_routeState != RouteNone && event->button() == Qt::LeftButton)
    {
        if (m_routeState == RouteMiddlePoints && !m_routeLayer->route().isEmpty())
            finishRoutePlanning();
        
        event->accept();
        return;
    }

    SsMapGraphicsView::mouseDoubleClickEvent(event);
}

void SsMultiMapView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu;

    if (m_measureState != MeasureNone)
    {
        QAction *cancelAction = menu.addAction(tr("取消测距"));
        connect(cancelAction, &QAction::triggered, this, &SsMultiMapView::cancelDistanceMeasurement);

        QAction *routeAction = menu.addAction(tr("路线规划"));
        connect(routeAction, &QAction::triggered, this, &SsMultiMapView::startRoutePlanning);
    }
    else if (m_routeState != RouteNone)
    {
        if (m_routeState == RouteMiddlePoints && !m_routeLayer->route().isEmpty())
        {
            QAction *finishAction = menu.addAction(tr("完成规划"));
            connect(finishAction, &QAction::triggered, this, &SsMultiMapView::finishRoutePlanning);
        }

        QAction *cancelAction = menu.addAction(tr("取消规划"));
        connect(cancelAction, &QAction::triggered, this, &SsMultiMapView::cancelRoutePlanning);

        QAction *exitAction = menu.addAction(tr("退出规划"));
        connect(exitAction, &QAction::triggered, this, &SsMultiMapView::exitRoutePlanning);

        QAction *measureAction = menu.addAction(tr("距离测量"));
        connect(measureAction, &QAction::triggered, this, &SsMultiMapView::startDistanceMeasurement);
    }
    else
    {
        QAction *measureAction = menu.addAction(tr("距离测量"));
        connect(measureAction, &QAction::triggered, this, &SsMultiMapView::startDistanceMeasurement);

        QAction *routeAction = menu.addAction(tr("路线规划"));
        connect(routeAction, &QAction::triggered, this, &SsMultiMapView::startRoutePlanning);
    }

    menu.exec(event->globalPos());
}

// ================= 测距辅助方法 =================
void SsMultiMapView::addMeasurePoint(const QGeoCoordinate &point)
{
    m_measurePoints.append(point);
    updateMeasurePath();

    if (m_tempLineItem)
    {
        scene()->removeItem(m_tempLineItem);
        delete m_tempLineItem;
        m_tempLineItem = nullptr;
    }
    if (m_tempDistanceText)
    {
        scene()->removeItem(m_tempDistanceText);
        delete m_tempDistanceText;
        m_tempDistanceText = nullptr;
    }
}

void SsMultiMapView::updateMeasurePath()
{
    // 如果没有测量点或只有1个点，清除现有路径并返回
    if (m_measurePoints.size() < 2)
    {
        if (m_measurePathItem)
        {
            scene()->removeItem(m_measurePathItem);
            delete m_measurePathItem;
            m_measurePathItem = nullptr;
        }
        
        for (auto textItem : m_distanceTextItems)
        {
            scene()->removeItem(textItem);
            delete textItem;
        }
        m_distanceTextItems.clear();
        return;
    }

    // 创建或更新路径
    QPainterPath path;
    QPointF firstPoint = geoToPixel(m_measurePoints.first());
    path.moveTo(firstPoint);

    // 确保文本项数量与线段数量匹配
    while (m_distanceTextItems.size() < m_measurePoints.size() - 1)
    {
        QGraphicsSimpleTextItem *textItem = new QGraphicsSimpleTextItem();
        QFont font = textItem->font();
        font.setBold(true);
        font.setPointSize(10);
        textItem->setFont(font);
        textItem->setBrush(Qt::yellow);
        textItem->setPen(QPen(Qt::black, 1));
        textItem->setZValue(1000);
        textItem->setFlags(QGraphicsItem::ItemIgnoresTransformations);
        scene()->addItem(textItem);
        m_distanceTextItems.append(textItem);
    }

    while (m_distanceTextItems.size() > m_measurePoints.size() - 1)
    {
        auto textItem = m_distanceTextItems.takeLast();
        scene()->removeItem(textItem);
        delete textItem;
    }

    // 更新路径和文本项
    for (int i = 1; i < m_measurePoints.size(); ++i)
    {
        QPointF currentPoint = geoToPixel(m_measurePoints[i]);
        path.lineTo(currentPoint);

        // 计算两点间距离
        double distance = calculateSegmentDistance(m_measurePoints[i - 1], m_measurePoints[i]);
        QPointF midPoint = (firstPoint + currentPoint) / 2;

        // 更新文本项
        m_distanceTextItems[i - 1]->setText(formatDistance(distance));
        m_distanceTextItems[i - 1]->setPos(midPoint);
        firstPoint = currentPoint;
    }

    // 更新或创建路径项
    if (!m_measurePathItem)
    {
        m_measurePathItem = scene()->addPath(path, QPen(Qt::red, 3));
        m_measurePathItem->setZValue(100);
    }
    else
    {
        m_measurePathItem->setPath(path);
    }
}

void SsMultiMapView::updateTempLine(const QGeoCoordinate &endPoint)
{
    // 创建或更新临时线
    QPointF startPixel = geoToPixel(m_measurePoints.last());
    QPointF endPixel = geoToPixel(endPoint);

    if (!m_tempLineItem)
    {
        m_tempLineItem = scene()->addLine(QLineF(startPixel, endPixel),
                                          QPen(Qt::red, 2, Qt::DashLine));
        m_tempLineItem->setZValue(100);
    }
    else
    {
        m_tempLineItem->setLine(QLineF(startPixel, endPixel));
    }

    // 计算并显示距离
    double distance = calculateSegmentDistance(m_measurePoints.last(), endPoint);
    QPointF midPoint = (startPixel + endPixel) / 2;

    if (!m_tempDistanceText)
    {
        m_tempDistanceText = scene()->addSimpleText(formatDistance(distance));
        QFont font = m_tempDistanceText->font();
        font.setBold(true);
        font.setPointSize(10);
        m_tempDistanceText->setFont(font);
        m_tempDistanceText->setPos(midPoint);
        m_tempDistanceText->setBrush(Qt::yellow);
        m_tempDistanceText->setPen(QPen(Qt::black, 1));
        m_tempDistanceText->setZValue(1000);
        m_tempDistanceText->setFlags(QGraphicsItem::ItemIgnoresTransformations);
    }
    else
    {
        m_tempDistanceText->setText(formatDistance(distance));
        m_tempDistanceText->setPos(midPoint);
    }
}

void SsMultiMapView::endDistanceMeasurement()
{
    double totalDistance = 0.0;
    for (int i = 1; i < m_measurePoints.size(); ++i)
    {
        totalDistance += calculateSegmentDistance(m_measurePoints[i - 1], m_measurePoints[i]);
    }
    emit measurementCompleted(totalDistance, m_measurePoints);
    m_measureState = MeasureEndPoint;
    setCursor(Qt::ArrowCursor);
}

void SsMultiMapView::clearMeasurement()
{
    m_measurePoints.clear();
    m_measureState = MeasureNone;

    if (m_measurePathItem)
    {
        scene()->removeItem(m_measurePathItem);
        delete m_measurePathItem;
        m_measurePathItem = nullptr;
    }

    for (auto textItem : m_distanceTextItems)
    {
        scene()->removeItem(textItem);
        delete textItem;
    }
    m_distanceTextItems.clear();

    if (m_tempLineItem)
    {
        scene()->removeItem(m_tempLineItem);
        delete m_tempLineItem;
        m_tempLineItem = nullptr;
    }

    if (m_tempDistanceText)
    {
        scene()->removeItem(m_tempDistanceText);
        delete m_tempDistanceText;
        m_tempDistanceText = nullptr;
    }
}

// ================= 路线规划辅助方法 =================
void SsMultiMapView::addRoutePoint(const QGeoCoordinate &point)
{
    QVector<QGeoCoordinate> route = m_routeLayer->route();
    route.append(point);
    m_routeLayer->setRoute(route);

    if (m_tempRouteLineItem)
    {
        scene()->removeItem(m_tempRouteLineItem);
        delete m_tempRouteLineItem;
        m_tempRouteLineItem = nullptr;
    }
}

void SsMultiMapView::updateTempRouteLine(const QGeoCoordinate &endPoint)
{
    if (m_routeLayer->route().isEmpty())
        return;

    QPointF startPixel = geoToPixel(m_routeLayer->route().last());
    QPointF endPixel = geoToPixel(endPoint);

    if (!m_tempRouteLineItem)
    {
        m_tempRouteLineItem = scene()->addLine(QLineF(startPixel, endPixel),
                                               QPen(Qt::blue, 2, Qt::DashLine));
        m_tempRouteLineItem->setZValue(100);
    }
    else
    {
        m_tempRouteLineItem->setLine(QLineF(startPixel, endPixel));
    }
}

void SsMultiMapView::clearRoute()
{
    m_routeLayer->clearRoute();

    if (m_tempRouteLineItem)
    {
        scene()->removeItem(m_tempRouteLineItem);
        delete m_tempRouteLineItem;
        m_tempRouteLineItem = nullptr;
    }

    viewport()->update();
}

// ================= 通用工具方法 =================
double SsMultiMapView::calculateSegmentDistance(const QGeoCoordinate &p1, const QGeoCoordinate &p2) const
{
    return p1.distanceTo(p2);
}

QString SsMultiMapView::formatDistance(double meters) const
{
    if(meters < 1000)
        return QString("%1 m").arg(qRound(meters));
    else
        return QString("%1 km").arg(meters / 1000, 0, 'f', 2);
}

void SsMultiMapView::wheelEvent(QWheelEvent *event)
{
    SsMapGraphicsView::wheelEvent(event);
    refreshMeasurementDisplay();
}

void SsMultiMapView::refreshMeasurementDisplay()
{
    if (m_measureState != MeasureNone && !m_measurePoints.isEmpty())
    {
        updateMeasurePath();
        if (m_tempLineItem)
        {
            QPointF endPos = mapToScene(viewport()->mapFromGlobal(QCursor::pos()));
            updateTempLine(pixelToGeo(endPos));
        }
    }
}