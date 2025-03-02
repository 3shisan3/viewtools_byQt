/*****************************************************************
File:        composite_translator.h
Version:     1.0
Author:
start date:
Description:
    复合翻译，需要单独进行语言转换时使用
Version history
[序号][修改日期][修改者][修改内容]

*****************************************************************/

#ifndef COMPOSITE_TRANSLATOR_H_
#define COMPOSITE_TRANSLATOR_H_

#include <QTranslator>

class SsCompositeTranslator : public QTranslator
{
    Q_OBJECT
public:
    explicit SsCompositeTranslator(QObject *parent = nullptr);
    
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override;
};

#endif