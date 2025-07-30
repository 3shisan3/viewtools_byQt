/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        online_tile_loader.h
Version:     1.0
Author:      cjx
start date: 
Description: 在线加载瓦片地图
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef ONLINE_TILE_LOADER_H
#define ONLINE_TILE_LOADER_H

#include <QMutex>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QThread>

struct DownTileInfo
{
    int x = 0;
    int y = 0;
    int z = 0;
    QString url;
    QString format;
    QPixmap img;
    short retryCount = 0;   // 重试次数
};

class SsOnlineTileLoader : public QThread
{
    Q_OBJECT
public:
    explicit SsOnlineTileLoader(QObject *parent = nullptr);
    ~SsOnlineTileLoader();

    void setUrlTemplate(const QString &templateStr, const QStringList &subdomains = QStringList());
    void requestTile(int x, int y, int z);
    void setMaxRetryCount(int count) { 
        QMutexLocker locker(&m_mutex);
        m_maxRetryCount = count; 
    }
    void setTimeout(int milliseconds) { 
        QMutexLocker locker(&m_mutex);
        m_timeout = milliseconds; 
    }

    void stop();

signals:
    void tileReceived(int x, int y, int z, const QPixmap &tile);
    void tileFailed(int x, int y, int z, const QString &error);

protected:
    void run() override;

private slots:
    void handleNetworkReply(QNetworkReply *reply);
    void handleRetry(int x, int y, int z);

private:
    void startRequest(int x, int y, int z);
    QNetworkReply* createRequest(int x, int y, int z);

    QMutex m_mutex;
    enum {
        Stopped,
        Running,
        Stopping
    } m_state;
    QNetworkAccessManager *m_networkManager;
    QString m_urlTemplate;
    QStringList m_subdomains;
    int m_maxRetryCount = 3; // 默认最大重试次数
    int m_timeout = 5000;    // 默认超时时间5秒
    QMap<QString, DownTileInfo> m_pendingRequests; // 记录正在处理的请求
};
#endif  // ONLINE_TILE_LOADER_H