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

uint mapSize(int level)
{
    return static_cast<uint>(256 * qPow(2.0, level));
}

qreal groundResolution(qreal lat, int level)
{
    return cos(lat * M_PI / 180.0) * 2 * M_PI * EARTH_EQUATOR_RADIUS / mapSize(level);
}

qreal mapScale(qreal lat, int level, int screenDpi)
{
    return groundResolution(lat, level) * screenDpi / 0.0254;
}

namespace Standard
{
QPoint latLongToPixelXY(qreal lon, qreal lat, int level)
{
    lon = TileForCoord::clipLon(lon);
    lat = TileForCoord::clipLat(lat);

    qreal x = (lon + 180) / 360;
    qreal sinLat = qSin(lat * M_PI / 180);
    qreal y = 0.5 - qLn((1 + sinLat) / (1 - sinLat)) / (4 * M_PI);

    uint size = TileForCoord::mapSize(level);
    qreal pixelX = x * size + 0.5;
    pixelX = TileForCoord::clip(pixelX, 0, size - 1);
    qreal pixelY = y * size + 0.5;
    pixelY = TileForCoord::clip(pixelY, 0, size - 1);

    return QPoint(pixelX, pixelY);
}

void pixelXYToLatLong(QPoint pos, int level, qreal &lon, qreal &lat)
{
    qreal mapSize = TileForCoord::mapSize(level);

    qreal x = (TileForCoord::clip(pos.x(), 0, mapSize - 1) / mapSize) - 0.5;
    lon = x * 360;

    qreal y = 0.5 - (pos.y() / mapSize);
    lat = 90.0 - 360.0 * atan(exp(-y * 2 * M_PI)) / M_PI;
}

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
    QPoint pixel = latLongToPixelXY(lon, lat, level);
    return QPoint(pixel.x() / 256, pixel.y() / 256);
}

/**
 * @brief 瓦片坐标转回经纬度
 * @param tile 瓦片XY坐标
 * @param level 缩放级别
 * @return 瓦片左上角对应的经纬度
 */
QPointF tileXYToLatLong(QPoint tile, int level)
{
    qreal lon, lat;
    pixelXYToLatLong(QPoint(tile.x() * 256, tile.y() * 256), level, lon, lat);
    return QPointF(lon, lat);
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