#include "online_tile_loader.h"
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <random>

SsOnlineTileLoader::SsOnlineTileLoader(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    connect(m_networkManager, &QNetworkAccessManager::finished,
            this, &SsOnlineTileLoader::handleNetworkReply);
}

SsOnlineTileLoader::~SsOnlineTileLoader()
{
    // 清理所有定时器
    for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end(); ++it)
    {
        if (it->retryTimer)
        {
            it->retryTimer->stop();
            delete it->retryTimer;
        }
    }
    m_pendingRequests.clear();
}

void SsOnlineTileLoader::setUrlTemplate(const QString &templateStr, const QStringList &subdomains)
{
    m_urlTemplate = templateStr;
    m_subdomains = subdomains;
}

void SsOnlineTileLoader::requestTile(int x, int y, int z)
{
    QString key = QString("%1-%2-%3").arg(x).arg(y).arg(z);

    // 如果已经有相同的请求在处理中，不再重复请求
    if (m_pendingRequests.contains(key))
    {
        return;
    }

    DownTileInfo info;
    info.x = x;
    info.y = y;
    info.z = z;
    info.retryCount = 0;
    m_pendingRequests.insert(key, info);

    startRequest(x, y, z);
}

void SsOnlineTileLoader::startRequest(int x, int y, int z)
{
    QNetworkReply *reply = createRequest(x, y, z);
    if (!reply)
        return;

    // 设置超时
    QTimer::singleShot(m_timeout, [this, reply]() {
        if (reply && reply->isRunning())
            reply->abort();
    });
}

QNetworkReply *SsOnlineTileLoader::createRequest(int x, int y, int z)
{
    QString key = QString("%1-%2-%3").arg(x).arg(y).arg(z);
    if (!m_pendingRequests.contains(key))
    {
        return nullptr;
    }

    DownTileInfo &info = m_pendingRequests[key];

    QString url = m_urlTemplate;
    url.replace("{x}", QString::number(x));
    url.replace("{y}", QString::number(y));
    url.replace("{z}", QString::number(z));

    // 随机选择子域名
    if (url.contains("{s}"))
    {
        if (!m_subdomains.isEmpty())
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(0, m_subdomains.size() - 1);
            int randomIndex = distrib(gen);
            url.replace("{s}", m_subdomains[randomIndex]);
        }
        else
        {
            url.replace("{s}", ""); // 如果没有子域名，移除占位符
        }
    }

    QNetworkRequest request;
    request.setUrl(QUrl(url));
    request.setAttribute(QNetworkRequest::User, QVariantList() << x << y << z << info.retryCount);

    return m_networkManager->get(request);
}

void SsOnlineTileLoader::handleNetworkReply(QNetworkReply *reply)
{
    QVariantList coords = reply->request().attribute(QNetworkRequest::User).toList();
    int x = coords[0].toInt();
    int y = coords[1].toInt();
    int z = coords[2].toInt();
    int retryCount = coords[3].toInt();

    QString key = QString("%1-%2-%3").arg(x).arg(y).arg(z);

    if (reply->error() == QNetworkReply::NoError)
    {
        QPixmap pixmap;
        if (pixmap.loadFromData(reply->readAll()))
        {
            emit tileReceived(x, y, z, pixmap);
            m_pendingRequests.remove(key); // 请求成功，移除记录
        }
        else
        {
            emit tileFailed(x, y, z, "Invalid image data");
            m_pendingRequests.remove(key); // 数据无效，不再重试
        }
    }
    else
    {
        if (retryCount < m_maxRetryCount)
        {
            // 延迟重试，避免立即重试导致服务器压力过大
            QTimer::singleShot(1000 * (retryCount + 1), this, [this, x, y, z]()
                               { handleRetry(x, y, z); });
        }
        else
        {
            emit tileFailed(x, y, z, reply->errorString());
            m_pendingRequests.remove(key); // 达到最大重试次数，移除记录
        }
    }

    reply->deleteLater();
}

void SsOnlineTileLoader::handleRetry(int x, int y, int z)
{
    QString key = QString("%1-%2-%3").arg(x).arg(y).arg(z);
    if (m_pendingRequests.contains(key))
    {
        DownTileInfo &info = m_pendingRequests[key];
        info.retryCount++;
        startRequest(x, y, z);
    }
}