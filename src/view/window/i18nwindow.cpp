#include "i18nwindow.h"

#include "utils/composite_translator.h"

SsI18nWindow::SsI18nWindow(const QString &context, QWidget *parent)
    : QWidget(parent), m_context(context)
{
    m_privateTranslator = new SsCompositeTranslator(this);
}

void SsI18nWindow::loadPrivateTranslation(const QString &qmPath)
{
    if (!m_privateTranslator->load(qmPath))
    {
        qWarning() << "Failed to load translation file:" << qmPath;
    }
}

QString SsI18nWindow::contextTr(const char *sourceText, const char *disambiguation, int n)
{
    return m_privateTranslator->translate(m_context.toUtf8(), sourceText, disambiguation, n);
}

void SsI18nWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
    {
        retranslateUi();
        // 递归更新子控件
        for (auto child : findChildren<QWidget *>())
        {
            QEvent childEvent(QEvent::LanguageChange);
            QCoreApplication::sendEvent(child, &childEvent);
        }
    }
    QWidget::changeEvent(event);
}