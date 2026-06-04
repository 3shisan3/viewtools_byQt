#pragma once

#include <QDir>
#include <QPixmap>

class SsDiskCacheManager : public QObject
{
public:
    explicit SsDiskCacheManager(const QString &cacheDir, QObject *parent = nullptr);

    bool hasTile(int x, int y, int z) const;
    bool saveTile(int x, int y, int z, const QPixmap &tile);
    QPixmap loadTile(int x, int y, int z) const;
    void clearCache();

    void setSaveDir(const QString &path);

    QString getCachePath(int x, int y, int z) const;

private:
    bool isSafeCacheDir(const QString &path) const;

    QString m_cacheDir;
};