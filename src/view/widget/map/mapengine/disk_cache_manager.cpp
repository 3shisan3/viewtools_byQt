#include "disk_cache_manager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QStandardPaths>

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
    QString path = getCachePath(x, y, z);
    QDir().mkpath(QFileInfo(path).absolutePath()); // 确保目录存在
    return tile.save(path, "PNG");
}

QPixmap SsDiskCacheManager::loadTile(int x, int y, int z) const
{
    QPixmap pixmap;
    pixmap.load(getCachePath(x, y, z));
    return pixmap;
}

void SsDiskCacheManager::setSaveDir(const QString &path)
{
    if (path.isEmpty() || !QDir().mkpath(path))
        return;

    m_cacheDir = path;
}

QString SsDiskCacheManager::getCachePath(int x, int y, int z) const
{
    return QString("%1/%2/%3/%4.png").arg(m_cacheDir).arg(z).arg(x).arg(y);
}

void SsDiskCacheManager::clearCache()
{
    if (!isSafeCacheDir(m_cacheDir))
        return;

    QDir(m_cacheDir).removeRecursively();
    QDir().mkpath(m_cacheDir);
}

bool SsDiskCacheManager::isSafeCacheDir(const QString &path) const
{
    if (path.isEmpty())
        return false;

    QDir dir(path);
    QString cleanPath = QDir::cleanPath(dir.absolutePath());
    QString appDir = QDir::cleanPath(QCoreApplication::applicationDirPath());
    QString appCacheDir = QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));

    if (cleanPath == QDir::rootPath() || cleanPath == appDir || cleanPath == appCacheDir)
        return false;

    return cleanPath.startsWith(appDir + "/") || cleanPath.startsWith(appCacheDir + "/");
}