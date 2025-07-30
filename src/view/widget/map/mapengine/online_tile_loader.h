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

class SsOnlineTileLoader : public QObject
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

    void start(); // 启动工作线程
    void stop();  // 停止工作线程

signals:
    void tileReceived(int x, int y, int z, const QPixmap &tile);
    void tileFailed(int x, int y, int z, const QString &error);
    void internalRequestTile(int x, int y, int z); // 内部信号，用于跨线程请求

private slots:
    void handleNetworkReply(QNetworkReply *reply);
    void processTileRequest(int x, int y, int z);

private:
    QNetworkReply *createRequest(int x, int y, int z);
    void handleRetry(int x, int y, int z);

    QMutex m_mutex;
    QThread *m_workerThread; // 工作线程指针
    bool m_running = false;
    QNetworkAccessManager *m_networkManager;
    QString m_urlTemplate;
    QStringList m_subdomains;
    int m_maxRetryCount = 3; // 默认最大重试次数
    int m_timeout = 5000;    // 默认超时时间5秒
    QMap<QString, DownTileInfo> m_pendingRequests;  // 记录正在处理的请求
};

#endif  // ONLINE_TILE_LOADER_H