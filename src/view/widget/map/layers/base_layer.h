#ifndef SSMAP_BASE_LAYER_H
#define SSMAP_BASE_LAYER_H

#include <QObject>
#include <QPainter>
#include <QGeoCoordinate>

#include "view/widget/map/coordinate/tile_coordinate_factory.h"

class BaseLayer : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数
     * @param parent 父对象
     */
    explicit BaseLayer(QObject* parent = nullptr);

    /**
     * @brief 纯虚函数，子类必须实现渲染逻辑
     * @param painter 绘图设备
     * @param viewport 视口大小
     * @param center 地图中心坐标
     * @param zoom 当前缩放级别
     */
    virtual void render(QPainter* painter, 
                       const QSize& viewport,
                       const QGeoCoordinate& center, 
                       double zoom,
                       const TileForCoord::TileAlgorithm& algorithm) = 0;

    /**
     * @brief 图层是否可见
     * @return 可见性状态
     */
    bool isVisible() const { return m_visible; }

    /**
     * @brief 设置图层可见性
     * @param visible 是否可见
     */
    void setVisible(bool visible) { m_visible = visible; }

signals:
    /**
     * @brief 当图层需要特定瓦片时触发
     * @param x 瓦片X坐标
     * @param y 瓦片Y坐标
     * @param z 瓦片Z坐标(缩放级别)
     */
    void tileRequired(int x, int y, int z);

    /**
     * @brief 当图层完成渲染时触发
     */
    void renderingComplete();

    /**
     * @brief 当图层需要重绘时触发
     */
    void updateRequested();

protected:
    bool m_visible = true;  // 图层可见性标志
};

#endif // SSMAP_BASE_LAYER_H