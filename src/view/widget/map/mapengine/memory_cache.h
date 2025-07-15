/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:        memory_cache.h
Version:     1.0
Author:      cjx
start date:
Description: 管理地图瓦片的内存缓存
            实现 LRU (最近最少使用) 缓存策略
            提供快速的瓦片存取接口
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef MAPENGINE_MEMORY_CACHE_H
#define MAPENGINE_MEMORY_CACHE_H

#include <QCache>
#include <QPixmap>

class SsMemoryCache
{
public:
    /**
     * @brief 构造函数
     * @param maxCostMB 最大缓存大小(MB)
     */
    explicit SsMemoryCache(int maxCostMB = 100);

    /**
     * @brief 检查缓存中是否存在指定瓦片
     */
    bool contains(int x, int y, int z) const;

    /**
     * @brief 插入瓦片到缓存
     */
    void insert(int x, int y, int z, const QPixmap &tile);

    /**
     * @brief 获取缓存中的瓦片
     */
    QPixmap get(int x, int y, int z) const;

    /**
     * @brief 清空缓存
     */
    void clear();

    /**
     * @brief 获取当前缓存使用量(MB)
     */
    double memoryUsage() const;

private:
    // 生成缓存键
    QString makeKey(int x, int y, int z) const;

    // 计算QPixmap的近似内存占用
    int calculatePixmapCost(const QPixmap &pixmap) const;

    QCache<QString, QPixmap> m_cache;
};

#endif // MAPENGINE_MEMORY_CACHE_H