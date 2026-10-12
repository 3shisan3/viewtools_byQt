/*****************************************************************
File:        dvd_utils.h
Version:     1.0
Author:      cjx
start date:
Description: DVD/VIDEO_TS 目录相关的通用工具
    1. 按 DVD 命名规则（VTS_xx_y.VOB）收集目录内 VOB 并排出自然播放顺序
    2. 支持跳过菜单/引导文件（VIDEO_TS.VOB、VTS_xx_0.VOB）
    播放窗口与转码窗口共用该逻辑

Version history
[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/

#ifndef _DVD_UTILS_H
#define _DVD_UTILS_H

#include <QString>
#include <QStringList>

class DvdUtils
{
public:
    /**
     * @brief 收集目录内的 VOB 文件，并按 DVD 正片顺序排序
     * @param dirPath DVD/VIDEO_TS 目录
     * @param skipMenus 是否跳过菜单/引导文件（VIDEO_TS.VOB、VTS_xx_0.VOB）
     * @return 排序后的 VOB 绝对路径列表（顺序即播放/合并顺序）
     *
     * 说明：若跳过菜单后为空（例如目录内只有菜单文件），会退回返回全部 VOB。
     */
    static QStringList collectVobSequence(const QString &dirPath, bool skipMenus);
};

#endif // _DVD_UTILS_H
