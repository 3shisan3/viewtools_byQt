#include "tile_coordinate.h"

namespace TileForCoord
{

/**
 * @brief 计算两点间的大圆距离（Haversine公式）
 * @param lon1 点1经度
 * @param lat1 点1纬度
 * @param lon2 点2经度
 * @param lat2 点2纬度
 * @return 两点间距离（米）
 * 
 * @note 公式原理：
 * a = sin²(Δφ/2) + cosφ1·cosφ2·sin²(Δλ/2)
 * c = 2·atan2(√a, √(1−a))
 * d = R·c
 */
qreal toDistance(qreal lon1, qreal lat1, qreal lon2, qreal lat2)
{
    // 实现Haversine公式计算两点间距离
    qreal dLat = qDegreesToRadians(lat2 - lat1);
    qreal dLon = qDegreesToRadians(lon2 - lon1);
    qreal a = qSin(dLat / 2) * qSin(dLat / 2) +
              qCos(qDegreesToRadians(lat1)) * qCos(qDegreesToRadians(lat2)) *
              qSin(dLon / 2) * qSin(dLon / 2);
    qreal c = 2 * qAtan2(qSqrt(a), qSqrt(1 - a));
    return EARTH_AVERAGE_RADIUS * c;
}

namespace Standard
{
/**
 * @brief 经纬度转瓦片坐标
 * @param lon 经度
 * @param lat 纬度
 * @param level 缩放级别（0-通常为世界视图）
 * @return 瓦片XY坐标（原点在左上角）
 * 
 * @note 算法流程：
 * 1. 标准化经度到[-180,180]，纬度到有效范围
 * 2. 经度→[0,1]范围：x = (lon + 180)/360
 * 3. 纬度→墨卡托投影y值：y = 0.5 - ln((1+sinφ)/(1-sinφ))/(4π)
 * 4. 根据级别缩放：tileX = floor(x * 2^level)
 */
QPoint latLongToTileXY(qreal lon, qreal lat, int level)
{
    // 输入标准化
    lon = clipLon(lon);
    lat = clipLat(lat);
    
    // 经度→[0,1]范围
    qreal x = (lon + 180.0) / 360.0;
    // 纬度→墨卡托投影y值
    qreal sinLat = qSin(qDegreesToRadians(lat));
    qreal y = 0.5 - qLn((1 + sinLat) / (1 - sinLat)) / (4 * M_PI);
    
    // 计算瓦片坐标（2^level = 1 << level）
    qreal mapSize = qPow(2.0, level);
    return QPoint(static_cast<int>(x * mapSize), static_cast<int>(y * mapSize));
}

/**
 * @brief 瓦片坐标转回经纬度
 * @param tile 瓦片XY坐标
 * @param level 缩放级别
 * @return 瓦片左上角对应的经纬度
 */
QPointF tileXYToLatLong(QPoint tile, int level)
{
    qreal mapSize = qPow(2.0, level);
    // 归一化到[0,1]范围
    qreal x = tile.x() / mapSize;
    qreal y = tile.y() / mapSize;
    
    // 反算经度
    qreal lon = x * 360.0 - 180.0;
    // 反算纬度（反双曲正切计算）
    qreal lat = 90.0 - qRadiansToDegrees(qAtan(qExp((0.5 - y) * 4 * M_PI)) * 2);
    
    return QPointF(lon, lat);
}

qreal groundResolution(qreal lat, int level)
{
    lat = clipLat(lat);
    return qCos(qDegreesToRadians(lat)) * 2 * M_PI * EARTH_EQUATOR_RADIUS / qPow(2.0, level + 8);
}

qreal mapScale(qreal lat, int level, int screenDpi)
{
    return groundResolution(lat, level) * screenDpi / 0.0254;
}

qreal toLat(qreal lon, qreal lat, int dis)
{
    // 实现纬度移动计算
    qreal d = dis / EARTH_AVERAGE_RADIUS;
    qreal newLat = lat + qRadiansToDegrees(d);
    return clipLat(newLat);
}

qreal toLon(qreal lon, qreal lat, int dis)
{
    // 实现经度移动计算
    qreal d = dis / (EARTH_AVERAGE_RADIUS * qCos(qDegreesToRadians(lat)));
    qreal newLon = lon + qRadiansToDegrees(d);
    return clipLon(newLon);
}

} // namespace Standard

namespace Bing
{

uint mapSize(int level)
{
    return static_cast<uint>(256 * qPow(2.0, level));
}

QPoint latLongToPixelXY(qreal lon, qreal lat, int level)
{
    lon = clipLon(lon);
    lat = clipLat(lat);
    
    qreal x = (lon + 180.0) / 360.0;
    qreal sinLat = qSin(qDegreesToRadians(lat));
    qreal y = 0.5 - qLn((1 + sinLat) / (1 - sinLat)) / (4 * M_PI);
    
    uint size = mapSize(level);
    return QPoint(static_cast<int>(x * size), static_cast<int>(y * size));
}

void pixelXYToLatLong(QPoint pos, int level, qreal &lon, qreal &lat)
{
    uint size = mapSize(level);
    qreal x = pos.x() / static_cast<qreal>(size);
    qreal y = pos.y() / static_cast<qreal>(size);
    
    lon = x * 360.0 - 180.0;
    lat = 90.0 - qRadiansToDegrees(qAtan(qExp((0.5 - y) * 4 * M_PI)) * 2);
}

QPoint latLongToTileXY(qreal lon, qreal lat, int level)
{
    QPoint pixel = latLongToPixelXY(lon, lat, level);
    return QPoint(pixel.x() / 256, pixel.y() / 256);
}

QPointF tileXYToLatLong(QPoint tile, int level)
{
    qreal lon, lat;
    pixelXYToLatLong(QPoint(tile.x() * 256, tile.y() * 256), level, lon, lat);
    return QPointF(lon, lat);
}

qreal groundResolution(qreal lat, int level)
{
    lat = clipLat(lat);
    return qCos(qDegreesToRadians(lat)) * 2 * M_PI * EARTH_EQUATOR_RADIUS / mapSize(level);
}

qreal mapScale(qreal lat, int level, int screenDpi)
{
    return groundResolution(lat, level) * screenDpi / 0.0254;
}

/**
 * @brief 生成QuadKey字符串
 * @param tile 瓦片坐标
 * @param level 缩放级别
 * @return QuadKey字符串（如"0230102"）
 * 
 * @note QuadKey生成规则：
 * 1. 将X和Y坐标的二进制位交错排列
 * 2. 每对XY位组合成1个数字（00→0, 01→1, 10→2, 11→3）
 * 3. 从最高位到最低位生成字符串
 */
QString tileXYToQuadKey(QPoint tile, int level)
{
    QString quadKey;
    for (int i = level; i > 0; --i)
    {
        char digit = '0';
        int mask = 1 << (i - 1);        // 从最高位开始处理

        // 检查X和Y的当前位
        if ((tile.x() & mask) != 0)
            digit++;

        if ((tile.y() & mask) != 0)
            digit += 2;

        quadKey.append(digit);
    }
    return quadKey;
}

void quadKeyToTileXY(QString quadKey, int &tileX, int &tileY, int &level)
{
    tileX = tileY = 0;
    level = quadKey.length();
    for (int i = level; i > 0; --i) {
        int mask = 1 << (i - 1);
        switch (quadKey[level - i].toLatin1()) {
        case '0':
            break;
        case '1':
            tileX |= mask;
            break;
        case '2':
            tileY |= mask;
            break;
        case '3':
            tileX |= mask;
            tileY |= mask;
            break;
        default:
            break;
        }
    }
}

} // namespace Bing

} // namespace TileForCoord