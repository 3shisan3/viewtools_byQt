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
    std::function<QPointF(double, double, int)> latLongToPixelXY;
    std::function<QPoint(double, double, int)> latLongToTileXY;
    std::function<void(QPointF, int, double &, double &)> pixelXYToLatLong;
    std::function<QPointF(QPoint, int)> tileXYToLatLong;
    std::function<double(double, int)> groundResolution;
    std::function<double(double, int, int)> mapScale;
    std::function<double(double, double, int)> toLat;
    std::function<double(double, double, int)> toLon;

    // 可选功能
    struct
    {
        std::function<QString(QPoint, int)> tileXYToQuadKey;
        std::function<void(QString, int &, int &, int &)> quadKeyToTileXY;
    } bingFeatures;

    // struct
    // {
    //     std::function<qreal(qreal, qreal, int)> toLat;
    //     std::function<qreal(qreal, qreal, int)> toLon;
    // } standardFeatures;

    // 功能支持检查
    bool supportsBingFeatures() const
    {
        return bingFeatures.tileXYToQuadKey && bingFeatures.quadKeyToTileXY;
    }

    // bool supportsStandardFeatures() const
    // {
    //     return standardFeatures.toLat && standardFeatures.toLon;
    // }
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
        TileAlgorithm algo;
    
        // 核心方法
        algo.latLongToPixelXY = Standard::latLongToPixelXY;
        algo.latLongToTileXY = Standard::latLongToTileXY;
        algo.pixelXYToLatLong = Standard::pixelXYToLatLong;
        algo.tileXYToLatLong = Standard::tileXYToLatLong;
        algo.groundResolution = TileForCoord::groundResolution;
        algo.mapScale = TileForCoord::mapScale;
        algo.toLat = Standard::toLat;
        algo.toLon = Standard::toLon;
        
        return algo; 
    }

    static TileAlgorithm createBing()
    {
        TileAlgorithm algo = createStandard();
        algo.bingFeatures.tileXYToQuadKey = Bing::tileXYToQuadKey;
        algo.bingFeatures.quadKeyToTileXY = Bing::quadKeyToTileXY;
        return algo;
    }
};

} // namespace TileForCoord

#endif // SSTILE_COORDINATE_FACTORY_H