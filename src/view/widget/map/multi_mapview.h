/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        multi_mapview.h
Version:     1.0
Author:      cjx
start date: 
Description: 混合多功能的地图界面
    包含测距，规划等功能
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef SSMULTI_MAPVIEW_H
#define SSMULTI_MAPVIEW_H

#include "map_graphicsview.h"

class SsMultiMapView : public SsMapGraphicsView
{
    Q_OBJECT
public:
    explicit SsMultiMapView(QWidget *parent = nullptr);
    ~SsMultiMapView() override;

    // 状态查询
    bool isMeasuring() const { return m_measureState != MeasureNone; }
    bool isRouting() const { return m_routeState != RouteNone; }

    // 路线样式设置
    void setRouteStyle(const QColor &color, int width, Qt::PenStyle style = Qt::SolidLine);

public slots:
    // 测距控制
    void startDistanceMeasurement();
    void cancelDistanceMeasurement();
    
    // 路线规划控制
    void startRoutePlanning();
    void cancelRoutePlanning();
    void finishRoutePlanning();
    void exitRoutePlanning();

signals:
    // 测距相关信号
    void measurementStarted();
    void measurementCanceled();
    void measurementCompleted(double totalDistance, const QList<QGeoCoordinate>& points);
    
    // 路线规划相关信号
    void routePlanningStarted();
    void routePlanningCanceled();
    void routePlanningFinished(const QVector<QGeoCoordinate> &route);
    void routePlanningExited();

protected:
    // 事件处理
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    enum MeasureState {
        MeasureNone,
        MeasureStartPoint,
        MeasureMiddlePoints,
        MeasureEndPoint
    };
    
    enum RouteState {
        RouteNone,
        RouteStartPoint,
        RouteMiddlePoints,
        RouteEndPoint
    };

    // 测距相关
    MeasureState m_measureState;
    QList<QGeoCoordinate> m_measurePoints;
    QGraphicsPathItem *m_measurePathItem;
    QList<QGraphicsSimpleTextItem*> m_distanceTextItems;
    QGraphicsLineItem *m_tempLineItem;
    QGraphicsSimpleTextItem *m_tempDistanceText;
    
    // 路线规划相关
    RouteState m_routeState;
    RouteLayer *m_routeLayer;
    QGraphicsLineItem *m_tempRouteLineItem;

    // 辅助方法
    void updateMeasurePath();
    void updateTempLine(const QGeoCoordinate &endPoint);
    void addMeasurePoint(const QGeoCoordinate &point);
    void endDistanceMeasurement();
    void clearMeasurement();
    void refreshMeasurementDisplay();
    
    void updateTempRouteLine(const QGeoCoordinate &endPoint);
    void addRoutePoint(const QGeoCoordinate &point);
    void clearRoute();
    
    double calculateSegmentDistance(const QGeoCoordinate &p1, const QGeoCoordinate &p2) const;
    QString formatDistance(double meters) const;
};

#endif // SSMULTI_MAPVIEW_H