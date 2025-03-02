#include "composite_translator.h"

#include <QCoreApplication>

SsCompositeTranslator::SsCompositeTranslator(QObject *parent)
    : QTranslator(parent) {}

QString SsCompositeTranslator::translate(const char *context, const char *sourceText,
                                         const char *disambiguation, int n) const
{
    // 先尝试私有翻译
    QString result = QTranslator::translate(context, sourceText, disambiguation, n);
    if (!result.isEmpty())
        return result;

    // 再尝试全局翻译
    result = QCoreApplication::translate(context, sourceText, disambiguation, n);
    return result;
}