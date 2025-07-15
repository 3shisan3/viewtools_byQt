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

#include <QNetworkAccessManager>
#include <QPixmap>

class SsOnlineTileLoader : public QObject
{
    Q_OBJECT
public:
    explicit SsOnlineTileLoader(QObject *parent = nullptr);

    void setUrlTemplate(const QString &templateStr);
    void requestTile(int x, int y, int z);

signals:
    void tileReceived(int x, int y, int z, const QPixmap &tile);
    void tileFailed(int x, int y, int z, const QString &error);

private slots:
    void handleNetworkReply(QNetworkReply *reply);

private:
    QNetworkAccessManager *m_networkManager;
    QString m_urlTemplate;
    QStringList m_subdomains{"a", "b", "c"};
};

#endif  // ONLINE_TILE_LOADER_H