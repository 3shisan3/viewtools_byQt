#ifdef EXAMPLE_ON

#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QFile>

#include "example.h"

ExampleWindow::ExampleWindow(QWidget *parent)
    : QWidget(parent)
{
    // 设置主窗口
    setWindowTitle("界面切换演示");
    resize(300, 200);

    // 创建按钮
    qmlBtn = new QPushButton("打开QML界面");
    qssBtn = new QPushButton("打开QSS界面");

    // 布局
    QVBoxLayout *layout = new QVBoxLayout;
    layout->addWidget(qmlBtn);
    layout->addWidget(qssBtn);
    setLayout(layout);

    // 连接信号槽
    connect(qmlBtn, &QPushButton::clicked, this, &ExampleWindow::showQmlWindow);
    connect(qssBtn, &QPushButton::clicked, this, &ExampleWindow::showQssWindow);
}

void ExampleWindow::showQmlWindow()
{
    // 创建QQuickWidget加载QML
    QQuickWidget *qmlWidget = new QQuickWidget;
    qmlWidget->setSource(QUrl("qrc:/ui/qml/example.qml"));
    qmlWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    qmlWidget->setWindowTitle("QML界面");
    qmlWidget->resize(400, 300);
    qmlWidget->show();
    this->hide();

    // 关闭窗口后，程序的主事件循环（QApplication::exec()）没有其他窗口或逻辑来维持运行，因此程序退出
}

void ExampleWindow::showQssWindow()
{
    // 创建QSS样式窗口
    QWidget *qssWindow = new QWidget;
    qssWindow->setWindowTitle("QSS界面");
    qssWindow->resize(400, 300);

    // 加载QSS样式
    QFile styleFile(":/ui/style_sheet/example.qss");
    styleFile.open(QFile::ReadOnly);
    QString styleSheet = QLatin1String(styleFile.readAll());
    qssWindow->setStyleSheet(styleSheet);

    // 添加示例控件
    QVBoxLayout *layout = new QVBoxLayout(qssWindow);
    QPushButton *btn = new QPushButton("样式按钮");
    QLineEdit *edit = new QLineEdit;
    QLabel *label = new QLabel("样式文本");

    layout->addWidget(btn);
    layout->addWidget(edit);
    layout->addWidget(label);

    qssWindow->show();
    this->hide();
}

#endif