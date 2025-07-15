#include "memory_cache.h"

#include <QDebug>

SsMemoryCache::SsMemoryCache(int maxCostMB)
{
    // 将MB转换为KB (Qt的缓存成本单位是KB)
    m_cache.setMaxCost(maxCostMB * 1024);
}

bool SsMemoryCache::contains(int x, int y, int z) const
{
    return m_cache.contains(makeKey(x, y, z));
}

void SsMemoryCache::insert(int x, int y, int z, const QPixmap &tile)
{
    if (tile.isNull())
        return;

    QString key = makeKey(x, y, z);
    int cost = calculatePixmapCost(tile);
    m_cache.insert(key, new QPixmap(tile), cost);
}

QPixmap SsMemoryCache::get(int x, int y, int z) const
{
    QPixmap *pixmap = m_cache[makeKey(x, y, z)];
    return pixmap ? *pixmap : QPixmap();
}

void SsMemoryCache::clear()
{
    m_cache.clear();
}

double SsMemoryCache::memoryUsage() const
{
    return m_cache.totalCost() / 1024.0; // 转换为MB
}

QString SsMemoryCache::makeKey(int x, int y, int z) const
{
    return QString("%1|%2|%3").arg(z).arg(x).arg(y);
}

int SsMemoryCache::calculatePixmapCost(const QPixmap &pixmap) const
{
    // 计算QPixmap内存占用的替代方案
    if (pixmap.isNull())
        return 0;

    // 近似计算: width * height * 4 (假设32位ARGB)
    // 除以1024转换为KB (QCache的成本单位)
    return (pixmap.width() * pixmap.height() * 4) / 1024;
}