/*****************************************************************
File:        settingmanager.h
Version:     1.0
Author:
start date:
Description:

Version history
[序号][修改日期][修改者][修改内容]

*****************************************************************/

#ifndef SETTING_MANAGER_H_
#define SETTING_MANAGER_H_

#include <QSettings>

#include "singleton.h"

class SsSettingsManager : public QSettings
{
    Q_OBJECT
public:
    friend SingletonTemplate<SsSettingsManager>;

private:
    explicit SsSettingsManager();

    static QString getConfigPath();
};

#endif // _SETTING_MANAGER_H_