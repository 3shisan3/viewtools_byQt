#include "dvd_utils.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QVector>

#include <algorithm>

namespace
{
struct DvdVobItem
{
    QString path;         // 绝对路径
    QString name;         // 文件名
    int title = 0;        // 标题号（VTS_xx）
    int seg = 0;          // 段落号（VTS_xx_y 中的 y）
    bool isMenu = false;  // 是否 DVD 菜单/引导文件
};
} // namespace

QStringList DvdUtils::collectVobSequence(const QString &dirPath, bool skipMenus)
{
    QDir dir(dirPath);
    if (!dir.exists())
        return QStringList();

    const QRegularExpression vtsRe("^VTS_(\\d+)_(\\d+)\\.vob$",
                                   QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression vmRe("^VIDEO_TS\\.vob$",
                                  QRegularExpression::CaseInsensitiveOption);

    QVector<DvdVobItem> items;
    const QFileInfoList infos = dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &fi : infos)
    {
        if (fi.suffix().compare("vob", Qt::CaseInsensitive) != 0)
            continue;

        DvdVobItem item;
        item.path = fi.absoluteFilePath();
        item.name = fi.fileName();

        const QRegularExpressionMatch vtsMatch = vtsRe.match(item.name);
        if (vtsMatch.hasMatch())
        {
            item.title = vtsMatch.captured(1).toInt();
            item.seg = vtsMatch.captured(2).toInt();
            item.isMenu = (item.seg == 0);
        }
        else if (vmRe.match(item.name).hasMatch())
        {
            // VIDEO_TS.VOB：主菜单/引导，归到最前
            item.title = -1;
            item.seg = -1;
            item.isMenu = true;
        }
        else
        {
            // 非标准命名的 VOB：放到标准正片之后（按文件名排序）
            item.title = 100000;
            item.seg = 100000;
        }
        items.append(item);
    }

    if (items.isEmpty())
        return QStringList();

    std::sort(items.begin(), items.end(), [](const DvdVobItem &a, const DvdVobItem &b) {
        if (a.title != b.title)
            return a.title < b.title;
        if (a.seg != b.seg)
            return a.seg < b.seg;
        return a.name < b.name;
    });

    // 全为菜单文件时保留全部内容，避免出现"无内容"
    int menuCount = 0;
    for (const DvdVobItem &it : items)
    {
        if (it.isMenu)
            menuCount++;
    }

    QStringList result;
    const bool doSkip = skipMenus && (menuCount < items.size());
    for (const DvdVobItem &it : items)
    {
        if (!(doSkip && it.isMenu))
            result << it.path;
    }
    return result;
}
