#ifndef I18NWINDOW_H
#define I18NWINDOW_H

#include <QWidget>
#include <QTranslator>
#include <QEvent>
#include <QCoreApplication>
#include <QApplication>

class SsCompositeTranslator;

class SsI18nWindow : public QWidget
{
    Q_OBJECT
public:
    explicit SsI18nWindow(const QString &context, QWidget *parent = nullptr);

    void loadPrivateTranslation(const QString &qmPath);
    virtual void retranslateUi() = 0;

protected:
    // 自定义类似Q_OBJECT生成的tr静态函数
    QString contextTr(const char *sourceText, const char *disambiguation = nullptr, int n = -1);
    void changeEvent(QEvent *event) override;

private:
    SsCompositeTranslator *m_privateTranslator;
    QString m_context;
};

#endif // I18NWINDOW_H