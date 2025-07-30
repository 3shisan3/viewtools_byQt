#include "online_tile_loader.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <random>

#include "view/widget/map/coordinate/tile_coordinate.h"

SsOnlineTileLoader::SsOnlineTileLoader(QObject *parent)
    : QObject(parent)
    , m_workerThread(nullptr)
    , m_networkManager(nullptr)
{
    // 网络管理器在主线程创建
    m_networkManager = new QNetworkAccessManager(this);
}

SsOnlineTileLoader::~SsOnlineTileLoader()
{
    stop();
}

void SsOnlineTileLoader::start()
{
    if (m_workerThread)
        return;

    m_workerThread = new QThread();
    this->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::started, this, []() {
        qDebug() << "Tile loader thread started";
    });

    // 在线程启动后建立连接
    connect(this, &SsOnlineTileLoader::internalRequestTile, this, &SsOnlineTileLoader::processTileRequest);

    connect(m_workerThread, &QThread::finished, m_workerThread, &QObject::deleteLater);

    m_workerThread->start();
    m_running = true;
}

void SsOnlineTileLoader::stop()
{
    if (!m_workerThread)
        return;

    m_running = false;
    m_workerThread->quit();
    m_workerThread->wait();
    m_workerThread = nullptr;
}

void SsOnlineTileLoader::setUrlTemplate(const QString &templateStr, const QStringList &subdomains)
{
    QMutexLocker locker(&m_mutex);
    m_urlTemplate = templateStr;
    m_subdomains = subdomains;
}

void SsOnlineTileLoader::requestTile(int x, int y, int z)
{
    if (!m_running)
        return;

    QString key = QString("%1-%2-%3").arg(x).arg(y).arg(z);

    QMutexLocker locker(&m_mutex);
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

    // 通过信号触发跨线程处理
    emit internalRequestTile(x, y, z);
}

void SsOnlineTileLoader::processTileRequest(int x, int y, int z)
{
    if (!m_running)
        return;

    QNetworkReply *reply = createRequest(x, y, z);
    if (!reply)
        return;

    // 使用QPointer自动检测对象是否存活
    QPointer<QNetworkReply> safeReply(reply);

    connect(safeReply, &QNetworkReply::finished, this, [this, safeReply]() {
        // 确保reply在作用域内有效
        if (safeReply) {
            handleNetworkReply(safeReply);
        }
    });

    // 设置超时
    QTimer::singleShot(m_timeout, [this, safeReply]() {
        if (safeReply && safeReply->isRunning())
            safeReply->abort();
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
    if (m_urlTemplate.contains("{x}"))   // XYZ格式
    {
        url.replace("{x}", QString::number(x));
        url.replace("{y}", QString::number(y));
        url.replace("{z}", QString::number(z));
    }
    else if (m_urlTemplate.contains("{q}"))   // Bing的quadKey格式
    {
        QPoint tile(x, y);
        QString quadKey = TileForCoord::Bing::tileXYToQuadKey(tile, z);
        url.replace("{q}", quadKey);
    }

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
    if (!m_running)
    {
        reply->deleteLater();
        return;
    }

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

            QMutexLocker locker(&m_mutex);
            m_pendingRequests.remove(key); // 请求成功，移除记录
        }
        else
        {
            emit tileFailed(x, y, z, "Invalid image data");

            QMutexLocker locker(&m_mutex);
            m_pendingRequests.remove(key); // 数据无效，不再重试
        }
    }
    else
    {
        if (retryCount < m_maxRetryCount)
        {
            QMetaObject::invokeMethod(this, [this, x, y, z]() {
                handleRetry(x, y, z);
            }, Qt::QueuedConnection);
        }
        else
        {
            emit tileFailed(x, y, z, reply->errorString());

            QMutexLocker locker(&m_mutex);
            m_pendingRequests.remove(key); // 达到最大重试次数，移除记录
        }
    }

    reply->deleteLater();
}

void SsOnlineTileLoader::handleRetry(int x, int y, int z)
{
    if (!m_running)
        return;

    QString key = QString("%1-%2-%3").arg(x).arg(y).arg(z);
    if (m_pendingRequests.contains(key))
    {
        DownTileInfo &info = m_pendingRequests[key];
        info.retryCount++;
        emit internalRequestTile(x, y, z);
    }
}