#include "multi_mapview.h"

#include <QGraphicsLineItem>
#include <QMenu>

SsMultiMapView::SsMultiMapView(QWidget *parent)
    : SsMapGraphicsView(parent)
    , m_measureState(MeasureNone)
    , m_measurePathItem(nullptr)
    , m_tempLineItem(nullptr)
    , m_tempDistanceText(nullptr)
{
}

SsMultiMapView::~SsMultiMapView()
{
    clearMeasurement();
}

bool SsMultiMapView::isMeasuring() const
{
    return m_measureState != MeasureNone;
}

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

void SsMultiMapView::wheelEvent(QWheelEvent *event)
{
    SsMapGraphicsView::wheelEvent(event);
    refreshMeasurementDisplay();
}

void SsMultiMapView::mousePressEvent(QMouseEvent *event)
{
    // 处理测距模式下的点击
    if (m_measureState != MeasureNone && event->button() == Qt::LeftButton)
    {
        QPointF scenePos = mapToScene(event->pos());
        QGeoCoordinate coord = pixelToGeo(scenePos);

        if (m_measureState == MeasureStartPoint)
        {
            addMeasurePoint(coord);
            m_measureState = MeasureMiddlePoints;
        }
        else if (m_measureState == MeasureMiddlePoints)
        {
            addMeasurePoint(coord);
        }

        event->accept();
        return;
    }

    // 非测距模式下的处理交给父类
    SsMapGraphicsView::mousePressEvent(event);
}

void SsMultiMapView::mouseMoveEvent(QMouseEvent *event)
{
    // 在测距模式下更新临时线
    if (m_measureState == MeasureMiddlePoints && !m_measurePoints.isEmpty())
    {
        QPointF scenePos = mapToScene(event->pos());
        QGeoCoordinate coord = pixelToGeo(scenePos);
        updateTempLine(coord);
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

    SsMapGraphicsView::mouseDoubleClickEvent(event);
}

void SsMultiMapView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu;

    if (m_measureState != MeasureNone)
    {
        QAction *cancelAction = menu.addAction(tr("Cancel Measurement"));
        connect(cancelAction, &QAction::triggered, this, [this]() {
            cancelDistanceMeasurement();
        });
    }
    else
    {
        QAction *measureAction = menu.addAction(tr("Distance Measurement"));
        connect(measureAction, &QAction::triggered, this, [this]() {
            startDistanceMeasurement();
        });
    }

    menu.exec(event->globalPos());
}

void SsMultiMapView::paintEvent(QPaintEvent *event)
{
    SsMapGraphicsView::paintEvent(event);

    // 在测距模式下绘制额外的图形
    if (m_measureState != MeasureNone)
    {
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);

        // 绘制测量点标记
        for (const auto &point : m_measurePoints)
        {
            QPointF pixelPos = geoToPixel(point);
            painter.setPen(QPen(Qt::red, 3));
            painter.setBrush(Qt::white);
            painter.drawEllipse(pixelPos, 6, 6);
        }
    }
}

void SsMultiMapView::addMeasurePoint(const QGeoCoordinate &point)
{
    m_measurePoints.append(point);
    updateMeasurePath();

    // 清除临时线
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
    // 清除之前的路径和文本
    if (m_measurePathItem)
    {
        scene()->removeItem(m_measurePathItem);
        delete m_measurePathItem;
    }

    for (auto textItem : m_distanceTextItems)
    {
        scene()->removeItem(textItem);
        delete textItem;
    }
    m_distanceTextItems.clear();

    if (m_measurePoints.size() < 2)
    {
        m_measurePathItem = nullptr;
        return;
    }

    // 创建新的路径
    QPainterPath path;
    QPointF firstPoint = geoToPixel(m_measurePoints.first());
    path.moveTo(firstPoint);

    // 绘制线段并计算每段距离
    for (int i = 1; i < m_measurePoints.size(); ++i)
    {
        QPointF currentPoint = geoToPixel(m_measurePoints[i]);
        path.lineTo(currentPoint);

        // 计算两点间距离
        double distance = calculateSegmentDistance(m_measurePoints[i - 1], m_measurePoints[i]);
        QPointF midPoint = (firstPoint + currentPoint) / 2;

        // 添加距离文本
        QGraphicsSimpleTextItem *textItem = scene()->addSimpleText(formatDistance(distance));
        QFont font = textItem->font();
        font.setBold(true);
        font.setPointSize(10);
        textItem->setFont(font);
        textItem->setPos(midPoint);
        textItem->setBrush(Qt::yellow);
        textItem->setPen(QPen(Qt::black, 1));
        textItem->setZValue(1000);
        textItem->setFlags(QGraphicsItem::ItemIgnoresTransformations);
        m_distanceTextItems.append(textItem);

        firstPoint = currentPoint;
    }

    // 添加路径到场景
    m_measurePathItem = scene()->addPath(path, QPen(Qt::red, 3));
    m_measurePathItem->setZValue(100);
}

void SsMultiMapView::updateTempLine(const QGeoCoordinate &endPoint)
{
    if (m_measurePoints.isEmpty())
        return;

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

    // 计算并显示临时距离
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

    qDebug() << "Measurement completed. Total distance:" << totalDistance << "meters";
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

double SsMultiMapView::calculateSegmentDistance(const QGeoCoordinate &p1, const QGeoCoordinate &p2) const
{
    return p1.distanceTo(p2);
}

QString SsMultiMapView::formatDistance(double meters) const
{
    if (meters < 1000)
        return QString("%1 m").arg(qRound(meters));
    else
        return QString("%1 km").arg(meters / 1000, 0, 'f', 2);
}