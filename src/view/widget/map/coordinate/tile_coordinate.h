/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
SPDX-License-Identifier: MIT 
File:        tile_coordinate.h
Version:     1.0
Author:      cjx
start date:
Description: 瓦片地图中坐标转换算法
    
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef SSTILE_COORDINATE_H
#define SSTILE_COORDINATE_H

#include <QPointF>
#include <QString>
#include <QtMath>
#include <cmath>

namespace TileForCoord
{

// 地球物理常量（单位：米）
constexpr qreal EARTH_EQUATOR_RADIUS = 6378137.0;       // 地球赤道半径
constexpr qreal EARTH_AVERAGE_RADIUS = 6371008.0;       // 地球平均半径
// 有效坐标范围（Web墨卡托投影限制）
constexpr qreal MinLatitude = -85.05112878;    // 纬度最小值（保证投影为正方形）
constexpr qreal MaxLatitude = 85.05112878;     // 纬度最大值
constexpr qreal MinLongitude = -180.0;         // 经度最小值
constexpr qreal MaxLongitude = 180.0;          // 经度最大值

/**
 * @brief 通用实用函数
 */
// 数值裁剪到指定范围
inline qreal clip(qreal n, qreal min, qreal max)
{
    return qBound(min, n, max);
}

// 经度标准化（-180到180之间）
inline qreal clipLon(qreal lon)
{
    return clip(lon, MinLongitude, MaxLongitude);
}

// 纬度标准化（Web墨卡托投影有效范围）
inline qreal clipLat(qreal lat)
{
    return clip(lat, MinLatitude, MaxLatitude);
}

// 计算两点间的大圆距离（Haversine公式）
qreal toDistance(qreal lon1, qreal lat1, qreal lon2, qreal lat2);

/**
 * @brief 计算指定级别的地图尺寸(像素)
 * @param level 缩放级别
 * @return 地图尺寸(像素)
 */
uint mapSize(int level);

/**
 * @brief 计算地面分辨率(米/像素)
 * @param lat 纬度
 * @param level 缩放级别
 * @return 地面分辨率
 */
qreal groundResolution(qreal lat, int level);

/**
 * @brief 计算地图比例尺
 * @param lat 纬度
 * @param level 缩放级别
 * @param screenDpi 屏幕DPI
 * @return 比例尺(1:xxx)
 */
qreal mapScale(qreal lat, int level, int screenDpi);

/**
 * @brief 标准瓦片算法实现
 */
namespace Standard
{
/**
 * @brief 经纬度转像素坐标
 * @param lon 经度
 * @param lat 纬度
 * @param level 缩放级别
 * @return 像素坐标
 */
QPoint latLongToPixelXY(qreal lon, qreal lat, int level);

/**
 * @brief 像素坐标转经纬度
 * @param pos 像素坐标
 * @param level 缩放级别
 * @param lon [out] 经度
 * @param lat [out] 纬度
 */
void pixelXYToLatLong(QPoint pos, int level, qreal &lon, qreal &lat);

// 经纬度转瓦片坐标
QPoint latLongToTileXY(qreal lon, qreal lat, int level);

// 瓦片坐标转经纬度
QPointF tileXYToLatLong(QPoint tile, int level);

// 移动指定距离后的新纬度
qreal toLat(qreal lon, qreal lat, int dis);

// 移动指定距离后的新经度
qreal toLon(qreal lon, qreal lat, int dis);

} // namespace Standard

/**
 * @brief Bing地图瓦片算法实现
 */
namespace Bing
{
// Bing特色功能：瓦片坐标转QuadKey
QString tileXYToQuadKey(QPoint tile, int level);

// Bing特色功能：QuadKey转瓦片坐标
void quadKeyToTileXY(QString quadKey, int &tileX, int &tileY, int &level);
}

/**
 * @brief 适用 天地图 瓦片地图的规则
 */
namespace CGCS2000_Rules
{
} // namespace CGCS2000_Rules

} // namespace TileForCoord

#endif  // SSTILE_COORDINATE_H