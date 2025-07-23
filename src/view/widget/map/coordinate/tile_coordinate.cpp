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
double toDistance(double lon1, double lat1, double lon2, double lat2)
{
    // 使用Vincenty公式提高精度
    double phi1 = qDegreesToRadians(lat1);
    double phi2 = qDegreesToRadians(lat2);
    double lambda1 = qDegreesToRadians(lon1);
    double lambda2 = qDegreesToRadians(lon2);
    
    double a = EARTH_EQUATOR_RADIUS;
    double b = EARTH_POLAR_RADIUS;
    double f = (a - b) / a;  // 扁率
    
    double L = lambda2 - lambda1;
    double U1 = atan((1 - f) * tan(phi1));
    double U2 = atan((1 - f) * tan(phi2));
    
    double sinU1 = sin(U1), cosU1 = cos(U1);
    double sinU2 = sin(U2), cosU2 = cos(U2);
    
    double lambda = L, lambdaP, iterLimit = 100;
    double cosSqAlpha, sinSigma, cosSigma, cos2SigmaM, sigma, sinLambda, cosLambda;
    
    do {
        sinLambda = sin(lambda);
        cosLambda = cos(lambda);
        sinSigma = sqrt((cosU2 * sinLambda) * (cosU2 * sinLambda) + 
                       (cosU1 * sinU2 - sinU1 * cosU2 * cosLambda) * 
                       (cosU1 * sinU2 - sinU1 * cosU2 * cosLambda));
        if (sinSigma == 0) return 0;  // 重合点
        
        cosSigma = sinU1 * sinU2 + cosU1 * cosU2 * cosLambda;
        sigma = atan2(sinSigma, cosSigma);
        double sinAlpha = cosU1 * cosU2 * sinLambda / sinSigma;
        cosSqAlpha = 1 - sinAlpha * sinAlpha;
        cos2SigmaM = cosSigma - 2 * sinU1 * sinU2 / cosSqAlpha;
        
        if (std::isnan(cos2SigmaM)) cos2SigmaM = 0;  // 赤道线
        
        double C = f / 16 * cosSqAlpha * (4 + f * (4 - 3 * cosSqAlpha));
        lambdaP = lambda;
        lambda = L + (1 - C) * f * sinAlpha * 
                (sigma + C * sinSigma * (cos2SigmaM + C * cosSigma * (-1 + 2 * cos2SigmaM * cos2SigmaM)));
    } while (fabs(lambda - lambdaP) > 1e-12 && --iterLimit > 0);
    
    if (iterLimit == 0) return 0;  // 公式不收敛
    
    double uSq = cosSqAlpha * (a * a - b * b) / (b * b);
    double A = 1 + uSq / 16384 * (4096 + uSq * (-768 + uSq * (320 - 175 * uSq)));
    double B = uSq / 1024 * (256 + uSq * (-128 + uSq * (74 - 47 * uSq)));
    double deltaSigma = B * sinSigma * (cos2SigmaM + B / 4 * 
                       (cosSigma * (-1 + 2 * cos2SigmaM * cos2SigmaM) - 
                       B / 6 * cos2SigmaM * (-3 + 4 * sinSigma * sinSigma) * 
                       (-3 + 4 * cos2SigmaM * cos2SigmaM)));
    
    double s = b * A * (sigma - deltaSigma);
    return s;
}

uint mapSize(int level)
{
    return static_cast<uint>(256 * qPow(2.0, level));
}

double groundResolution(double lat, int level)
{
    lat = clipLat(lat);
    return cos(lat * M_PI / 180.0) * 2 * M_PI * EARTH_EQUATOR_RADIUS / mapSize(level);
}

double mapScale(double lat, int level, int screenDpi)
{
    return groundResolution(lat, level) * screenDpi / 0.0254;
}

namespace Standard
{
QPointF latLongToPixelXY(double lon, double lat, int level)
{
    lon = clipLon(lon);
    lat = clipLat(lat);

    // 优化墨卡托投影计算
    double x = (lon + 180.0) / 360.0;
    
    // 使用更精确的墨卡托投影公式
    double sinLat = sin(lat * M_PI / 180.0);
    double y = 0.5 - log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * M_PI);

    uint size = mapSize(level);
    double pixelX = x * size;
    double pixelY = y * size;
    
    // 边界处理（不再需要round，保留小数部分）
    pixelX = clip(pixelX, 0.0, size - 1.0);
    pixelY = clip(pixelY, 0.0, size - 1.0);

    return QPointF(pixelX, pixelY);
}

void pixelXYToLatLong(QPointF pos, int level, double &lon, double &lat)
{
    double mapSize = static_cast<double>(TileForCoord::mapSize(level));
    
    // 使用双精度计算
    double x = (clip(pos.x(), 0.0, mapSize - 1.0) / mapSize) - 0.5;
    lon = x * 360.0;

    double y = 0.5 - (clip(pos.y(), 0, static_cast<int>(mapSize - 1)) / mapSize);
    // 使用更精确的反墨卡托投影
    lat = 90.0 - 360.0 * atan(exp(-y * 2.0 * M_PI)) / M_PI;
}

QPoint latLongToTileXY(double lon, double lat, int level)
{
    QPointF pixel = latLongToPixelXY(lon, lat, level);
    return QPoint(static_cast<int>(floor(pixel.x() / 256.0)),
                 static_cast<int>(floor(pixel.y() / 256.0)));
}

QPointF tileXYToLatLong(QPoint tile, int level)
{
    double lon, lat;
    pixelXYToLatLong(QPointF(tile.x() * 256.0, tile.y() * 256.0), level, lon, lat);
    return QPointF(lon, lat);
}

double toLat(double lon, double lat, int dis)
{
    // 实现纬度移动计算
    double d = dis / EARTH_AVERAGE_RADIUS;
    double newLat = lat + qRadiansToDegrees(d);
    return clipLat(newLat);
}

double toLon(double lon, double lat, int dis)
{
    // 实现经度移动计算
    double d = dis / (EARTH_AVERAGE_RADIUS * cos(qDegreesToRadians(lat)));
    double newLon = lon + qRadiansToDegrees(d);
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