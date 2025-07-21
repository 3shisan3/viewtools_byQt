/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
SPDX-License-Identifier: MIT 
File:        tile_coordinate_factory.h
Version:     1.0
Author:      cjx
start date:
Description: 基于瓦片坐标转换算法的策略(函数对象)工厂模式实现
    
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef SSTILE_COORDINATE_FACTORY_H
#define SSTILE_COORDINATE_FACTORY_H

#include "tile_coordinate.h"

#include <functional>
#include <memory>
#include <type_traits>

namespace TileForCoord
{

class TileAlgorithm
{
public:
    // 核心功能
    std::function<QPoint(qreal, qreal, int)> latLongToTileXY;
    std::function<QPointF(QPoint, int)> tileXYToLatLong;
    std::function<qreal(qreal, int)> groundResolution;
    std::function<qreal(qreal, int, int)> mapScale;

    // 可选功能
    struct
    {
        std::function<QString(QPoint, int)> tileXYToQuadKey;
        std::function<void(QString, int &, int &, int &)> quadKeyToTileXY;
    } bingFeatures;

    struct
    {
        std::function<qreal(qreal, qreal, int)> toLat;
        std::function<qreal(qreal, qreal, int)> toLon;
    } standardFeatures;

    // 功能支持检查
    bool supportsBingFeatures() const
    {
        return bingFeatures.tileXYToQuadKey && bingFeatures.quadKeyToTileXY;
    }

    bool supportsStandardFeatures() const
    {
        return standardFeatures.toLat && standardFeatures.toLon;
    }
};

class TileAlgorithmFactory
{
public:
    enum class AlgorithmType
    {
        Standard,
        Bing
    };

    static TileAlgorithm create(AlgorithmType type)
    {
        switch (type)
        {
        case AlgorithmType::Standard:
            return createStandard();
        case AlgorithmType::Bing:
            return createBing();
        default:
            return createStandard();
        }
    }

private:
    static TileAlgorithm createStandard()
    {
        return {
            Standard::latLongToTileXY,
            Standard::tileXYToLatLong,
            Standard::groundResolution,
            Standard::mapScale,
            {}, // bingFeatures
            {   // standardFeatures
             Standard::toLat,
             Standard::toLon}};
    }

    static TileAlgorithm createBing()
    {
        return {
            Bing::latLongToTileXY,
            Bing::tileXYToLatLong,
            Bing::groundResolution,
            Bing::mapScale,
            {// bingFeatures
             Bing::tileXYToQuadKey,
             Bing::quadKeyToTileXY},
            {} // standardFeatures
        };
    }
};

} // namespace TileForCoord

#endif // SSTILE_COORDINATE_FACTORY_H