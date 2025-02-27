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