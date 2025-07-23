#pragma once

#include <QPixmap>
#include <QDir>

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
    QString m_cacheDir;
};