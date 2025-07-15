#include "online_tile_loader.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <random>

SsOnlineTileLoader::SsOnlineTileLoader(QObject *parent)
    : QObject(parent)
{
    m_networkManager = new QNetworkAccessManager(this);
    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, &SsOnlineTileLoader::handleNetworkReply);
}

void SsOnlineTileLoader::setUrlTemplate(const QString &templateStr)
{
    m_urlTemplate = templateStr;
}

void SsOnlineTileLoader::requestTile(int x, int y, int z)
{
    QString url = m_urlTemplate;
    url.replace("{x}", QString::number(x));
    url.replace("{y}", QString::number(y));
    url.replace("{z}", QString::number(z));
    // ​​随机选择子域名​​, 以平衡服务器负载
    std::random_device rd;  // 随机设备种子
    std::mt19937 gen(rd()); // Mersenne Twister 引擎
    std::uniform_int_distribution<> distrib(0, m_subdomains.size() - 1);
    int randomIndex = distrib(gen);
    url.replace("{s}", m_subdomains[randomIndex]);

    QNetworkRequest request;
    request.setUrl(QUrl(url));
    request.setAttribute(QNetworkRequest::User, QVariantList() << x << y << z);

    m_networkManager->get(request);
}

void SsOnlineTileLoader::handleNetworkReply(QNetworkReply *reply)
{
    QVariantList coords = reply->request().attribute(QNetworkRequest::User).toList();
    int x = coords[0].toInt();
    int y = coords[1].toInt();
    int z = coords[2].toInt();

    if (reply->error() == QNetworkReply::NoError)
    {
        QPixmap pixmap;
        if (pixmap.loadFromData(reply->readAll()))
        {
            emit tileReceived(x, y, z, pixmap);
        }
        else
        {
            emit tileFailed(x, y, z, "Invalid image data");
        }
    }
    else
    {
        emit tileFailed(x, y, z, reply->errorString());
    }

    reply->deleteLater();
}