/*****************************************************************
File:        translation_manager.h
Version:     1.0
Author:
start date:
Description:
    可拓展，将不同窗口的语言包拆分管理，窗口收到languageChange信号，统一从此类获取分配
Version history
[序号][修改日期][修改者][修改内容]

*****************************************************************/

#ifndef TRANSLATION_MANAGER_H_
#define TRANSLATION_MANAGER_H_

#include <QFileInfo>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QTranslator>

#include "singleton.h"

class SsTranslationManager : public QObject
{
    Q_OBJECT
public:
    friend SingletonTemplate<SsTranslationManager>;

    // 获取语言包列表（供语言切换控件使用）
    QList<QString> getLanguages() const;

    // 获取当前使用的语言
    QString getCurrentLanguage() const;


private Q_SLOTS:
    // 加载语言包（外部引入新的语言包）
    void load(const QString& path);
    // 切换全局的上的语言包
    void change(const QString& target);

private:
    explicit SsTranslationManager(QObject *parent = nullptr);

    // 返回值表示当前所用语言是否有更新
    bool loadFile(const QFileInfo& path);
    bool loadDir(const QString& path);

    mutable QMutex m_mutex_;                    // 互斥锁
    QString m_curLanguage_;                     // 配置文件中的语言
    QMap<QString, QString> m_mapLanguage_;      // 当前可用的语言包列表
    std::unique_ptr<QTranslator> m_translator_; // 当前全局上所用翻译器
};

#endif // _TRANSLATION_MANAGER_H_