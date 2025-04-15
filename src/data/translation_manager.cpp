#include "translation_manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>

#include "event/event_dispatcher.h"
#include "settings_manager.h"

static const QString DEFAULT_LANGUAGE = "English";
static const QString DEFAULT_LANGUAGE_SAVEDIR = ":/ui/i18n";
// 语言包 -> 本地化语言名称（也可考虑用tr()代替value部分）
static QMap<QString, QString> langCodeToName = {
    {"zh_CN", "简体中文"},       // 中文界面显示
    {"en_US", "English"},       // 英文界面显示
    {"ja_JP", "日本語"},         // 日文界面显示
    {"ko_KR", "한국어"},         // 韩文界面显示
    {"fr_FR", "Français"},      // 法文界面显示
    {"es_ES", "Español"},       // 西班牙文界面显示
    {"de_DE", "Deutsch"},       // 德文界面显示
    {"ru_RU", "Русский"}        // 俄文界面显示
};

SsTranslationManager::SsTranslationManager(QObject *parent)
    : QObject(parent)
{
    // 获取本地配置是否指定了语言包
    auto &cfg = SingletonTemplate<SsSettingsManager>::getSingletonInstance();
    m_curLanguage_ = cfg.value("language", DEFAULT_LANGUAGE).toString();
    m_mapLanguage_.insert(m_curLanguage_, "");

    // 加载本地默认安装有的语言包
    loadDir(DEFAULT_LANGUAGE_SAVEDIR);
    
    auto &event = SingletonTemplate<EventDispatcher>::getSingletonInstance();
    connect(&event, &EventDispatcher::switchLanguage, this, &SsTranslationManager::change);
    connect(&event, &EventDispatcher::loadLanguageBag, this, &SsTranslationManager::load);
}

QList<QString> SsTranslationManager::getLanguages() const
{
    QMutexLocker lock(&m_mutex_);

    return m_mapLanguage_.keys();
}

QString SsTranslationManager::getCurrentLanguage() const
{
    return m_curLanguage_;
}

void SsTranslationManager::load(const QString &path)
{
    bool isUpdate = false;

    QFileInfo fileInfo(path);
    // 处理符号链接
    while (fileInfo.isSymLink())
    {
        fileInfo = fileInfo.symLinkTarget();
    }
    // 判断路径有效性及是否为文件夹
    if (!fileInfo.exists())
    {
        qDebug() << "load language qm path is not exists!";
    }
    else if (fileInfo.isDir())
    {
        isUpdate = loadDir(path);
    }
    else if (fileInfo.isFile())
    {
        isUpdate = loadFile(fileInfo);
    }

    // 触发界面重载翻译
    if(isUpdate)
    {
        // 使用changeEvent槽，也可结合EventDispatcher自定义，看情况。
        // 触发所有窗口重新翻译 (QApplication::sendEvent(window, event);指定控件)
        QApplication::sendEvent(QApplication::instance(), new QEvent(QEvent::LanguageChange));
    }
}

void SsTranslationManager::change(const QString& target)
{
    if (target == m_curLanguage_ || m_mapLanguage_.find(target) == m_mapLanguage_.end())
    {
        return;
    }

    QTranslator *translator = new QTranslator;
    QString path = m_mapLanguage_[target];
    if (!translator->load(path))
    {
        delete translator;
        translator = NULL;
        qWarning() << QString("[%1] load fail!").arg(path);
        return;
    }

    if (!QCoreApplication::installTranslator(translator))
    {
        qCritical() << "[Translation] Install failed";
        delete translator;
        translator = NULL;
    }
    else
    {
        if (m_translator_ != nullptr)
        {
            QCoreApplication::removeTranslator(m_translator_.get());
        }
        m_translator_.reset(translator);
        m_curLanguage_ = target;

        // 更新配置文件中数据
        SingletonTemplate<SsSettingsManager>::getSingletonInstance().setValue("language", m_curLanguage_);
    }

    // 触发所有窗口重新翻译 (QApplication::sendEvent(window, event);指定控件)
    QApplication::sendEvent(QApplication::instance(), new QEvent(QEvent::LanguageChange));
}

bool SsTranslationManager::loadFile(const QFileInfo &file)
{
    bool isUpdate = false;

    QString languageName = langCodeToName.find(file.baseName()) == langCodeToName.end() ?
        file.baseName() : langCodeToName[file.baseName()];

    QMutexLocker lock(&m_mutex_);
    QTranslator *translator = new QTranslator;
    if (!translator->load(file.absoluteFilePath()))
    {
        delete translator;
        translator = NULL;
        qWarning() << QString("[%1] load fail!").arg(file.absoluteFilePath());
        return isUpdate;
    }

    m_mapLanguage_.insert(languageName, file.absoluteFilePath());
    if (languageName == m_curLanguage_)
    {
        // 安装新翻译器，安装失败，旧的仍会保留（也是内存泄漏可能产生的原因），故不考虑回滚
        if (!QCoreApplication::installTranslator(translator))
        {
            qCritical() << "[Translation] Install failed";
            delete translator;
            translator = NULL;
        }
        else
        {
            // 先移除旧翻译器
            if (m_translator_ != nullptr)
            {
                QCoreApplication::removeTranslator(m_translator_.get());
            }
            // 转移所有权到unique_ptr
            m_translator_.reset(translator); // 自动删除旧实例
            isUpdate = true;
        }
    }
    else
    {
        // 不需要的翻译器立即删除
        delete translator;
    }

    return isUpdate;
}

bool SsTranslationManager::loadDir(const QString &path)
{
    QStringList nameFilters;
    nameFilters.append("*.qm");

    const QFileInfoList qmInfoList = QDir(path).entryInfoList(nameFilters, QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    bool isUpdate = false;
    QMutexLocker lock(&m_mutex_);
    foreach (const QFileInfo qmFileInfo, qmInfoList)
    {
        QString languageName = langCodeToName.find(qmFileInfo.baseName()) == langCodeToName.end() ?
            qmFileInfo.baseName() : langCodeToName[qmFileInfo.baseName()];

        m_mapLanguage_.insert(languageName, qmFileInfo.absoluteFilePath());
        // 不是当前所用语言，不需要QTranslator加载
        if (languageName != m_curLanguage_)
        {
            continue;
        }

        QTranslator *translator = new QTranslator;
        if(!translator->load(qmFileInfo.absoluteFilePath()))
        {
            delete translator;
            translator = NULL;

            qWarning()<< QString("[%1] load fail!").arg(qmFileInfo.absoluteFilePath());
            continue;
        }

        if (!QCoreApplication::installTranslator(translator))
        {
            qCritical() << "[Translation] Install failed";
        }
        else
        {
            // 先移除旧翻译器
            if (m_translator_ != nullptr)
            {
                QCoreApplication::removeTranslator(m_translator_.get());
            }
            // 转移所有权到unique_ptr
            m_translator_.reset(translator); // 自动删除旧实例
            isUpdate = true;
        }
    }

    return isUpdate;
}