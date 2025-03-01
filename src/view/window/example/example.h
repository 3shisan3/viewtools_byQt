/***************************************************************
Copyright (c) 2022-2030, shisan233@sszc.live.
All rights reserved.
File:
Version:     1.0
Author:      cjx
start date: 
Description: 范例界面
Version history

[序号]    |   [修改日期]  |   [修改者]   |   [修改内容]

*****************************************************************/
#ifdef EXAMPLE_ON   // qt中继承qobject；使用相关宏的情况下moc会加入生成文件，故功能未开启此头文件仍会被引用，导致报错

#ifndef _EXAMPLEWINDOW_H
#define _EXAMPLEWINDOW_H

#include <QWidget>
#include <QPushButton>
#include <QQuickWidget>
#include <QApplication>

class ExampleWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ExampleWindow(QWidget *parent = nullptr);

private slots:
    void showQmlWindow();
    void showQssWindow();

private:
    QPushButton *qmlBtn;
    QPushButton *qssBtn;
};

#endif // _EXAMPLEWINDOW_H

#endif // EXAMPLE_ON