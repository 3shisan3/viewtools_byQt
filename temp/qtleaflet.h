#ifndef QTLEAFLET_H
#define QTLEAFLET_H

#include <QWidget>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QCache>
#include <QMap>
#include <QPixmap>
#include <QPointF>
#include <QUrl>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QRandomGenerator>
#include <QFileInfo>

class TileLoader : public QObject
{
    Q_OBJECT
public:
    enum TileSourceType {
        RemoteHttp,  // 远程HTTP源
        LocalFile    // 本地文件源
    };

    explicit TileLoader(QObject *parent = nullptr);
    ~TileLoader();
    
    void setTileSource(const QString &source, TileSourceType type);
    QString tileSource() const;
    TileSourceType sourceType() const;
    
    void requestTile(const QPoint &tilePos, int zoom);
    
    QNetworkAccessManager* networkManager() const { return m_networkManager; }
    
signals:
    void tileLoaded(const QPixmap &pixmap, const QPoint &tilePos, int zoom);
    void debugMessage(const QString &message);
    
private slots:
    void handleNetworkReply(QNetworkReply *reply);
    
private:
    QNetworkAccessManager *m_networkManager;
    QString m_tileSource;
    TileSourceType m_sourceType;
    QMap<QUrl, QPair<QPoint, int>> m_activeRequests;
};

class TileCache : public QObject
{
    Q_OBJECT
public:
    explicit TileCache(QObject *parent = nullptr);
    ~TileCache();
    
    void setCachePath(const QString &path);
    QString cachePath() const;
    bool hasTile(const QString &key) const;
    bool getTile(const QString &key, QPixmap *pixmap) const;
    void putTile(const QString &key, const QPixmap &pixmap);
    
    QString tilePath(const QString &key) const;
    
private:
    QString m_cachePath;
    mutable QCache<QString, QPixmap> m_memoryCache;
};

class QtLeaflet : public QGraphicsView
{
    Q_OBJECT
public:
    enum TileProvider {
        OpenStreetMap,
        GoogleMap,
        LocalFileSystem
    };

    explicit QtLeaflet(QWidget *parent = nullptr);
    ~QtLeaflet();

    // 地图操作
    void setCenter(double lat, double lng);
    void setZoom(int zoom);
    int zoom() const;
    
    // 瓦片源配置
    void setTileSource(TileProvider provider);
    void setCustomTileSource(const QString &source, TileLoader::TileSourceType type);
    QString tileSource() const;
    
    // 缓存配置
    void setCacheEnabled(bool enabled);
    void setCachePath(const QString &path);
    QString cachePath() const;
    
    // 添加标记
    void addMarker(double lat, double lng, const QPixmap &icon);
    void clearMarkers();

signals:
    void debugMessage(const QString &message);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void handleTileLoaded(const QPixmap &pixmap, const QPoint &tilePos, int zoom);
    void updateTiles();

private:
    void initScene();
    void loadVisibleTiles();
    QPointF latLngToScenePos(double lat, double lng) const;
    QPointF scenePosToLatLng(const QPointF &pos) const;
    QPoint latLngToTilePos(double lat, double lng, int zoom) const;
    QPointF tilePosToScenePos(const QPoint &tilePos) const;
    QString tileKey(const QPoint &tilePos, int zoom) const;

    QGraphicsScene *m_scene;
    TileLoader *m_tileLoader;
    TileCache *m_tileCache;
    
    double m_centerLat;
    double m_centerLng;
    int m_zoom;
    
    QPoint m_lastMousePos;
    bool m_dragging;
    
    QMap<QString, QGraphicsPixmapItem*> m_markers;
};

#endif // QTLEAFLET_H