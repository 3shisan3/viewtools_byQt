#include "disk_cache_manager.h"

#include <QDateTime>

SsDiskCacheManager::SsDiskCacheManager(const QString &cacheDir, QObject *parent)
    : QObject(parent), m_cacheDir(cacheDir)
{
    QDir().mkpath(m_cacheDir);
}

bool SsDiskCacheManager::hasTile(int x, int y, int z) const
{
    return QFile::exists(getCachePath(x, y, z));
}

bool SsDiskCacheManager::saveTile(int x, int y, int z, const QPixmap &tile)
{
    return tile.save(getCachePath(x, y, z), "PNG");
}

QPixmap SsDiskCacheManager::loadTile(int x, int y, int z) const
{
    QPixmap pixmap;
    pixmap.load(getCachePath(x, y, z));
    return pixmap;
}

QString SsDiskCacheManager::getCachePath(int x, int y, int z) const
{
    return QString("%1/%2_%3_%4.png").arg(m_cacheDir).arg(z).arg(x).arg(y);
}

void SsDiskCacheManager::clearCache()
{
    QDir(m_cacheDir).removeRecursively();
    QDir().mkpath(m_cacheDir);
}