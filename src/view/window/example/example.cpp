#ifdef EXAMPLE_ON

#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QFile>

#include "example.h"
#include "data/translation_manager.h"
#include "event/event_dispatcher.h"
#ifdef MEDIA_PLAYER_ENABLE
#include "view/window/player/player_window.h"
#endif
#include "view/component/joystick_wheel.h"

ExampleWindow::ExampleWindow(QWidget *parent)
    : QWidget(parent)
{
    // 设置主窗口
    setWindowTitle(tr("Interface switching demo"));
    resize(300, 200);

    // 创建按钮
    qmlBtn = new QPushButton(tr("open qml window"));
    qssBtn = new QPushButton(tr("open qss window"));

    // 布局
    QVBoxLayout *layout = new QVBoxLayout;
    layout->addWidget(qmlBtn);
    layout->addWidget(qssBtn);
    setLayout(layout);

    // 连接信号槽
    connect(qmlBtn, &QPushButton::clicked, this, &ExampleWindow::showQmlWindow);
    connect(qssBtn, &QPushButton::clicked, this, &ExampleWindow::showQssWindow);

    // 额外功能界面添加
    extraFeatures();

    // 获取测试使用的语言包名，并去进行选择
    QString languageName = SingletonTemplate<SsTranslationManager>::getSingletonInstance().getLanguages().back();
    emit SingletonTemplate<EventDispatcher>::getSingletonInstance().switchLanguage(languageName);
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

void ExampleWindow::extraFeatures()
{
    QVBoxLayout *layout = qobject_cast<QVBoxLayout*>(this->layout());
    if(!layout) return;

#ifdef MEDIA_PLAYER_ENABLE
    QPushButton *playerBtn = new QPushButton(tr("open player window"));
    layout->addWidget(playerBtn);
    connect(playerBtn, &QPushButton::clicked, this, [this]() {
#ifdef CAN_USE_FFMPEG
        PlayerWindow *playerWindow = new PlayerWindow(nullptr, PlayerWidget::BY_FFMPEG);
#else
        PlayerWindow *playerWindow = new PlayerWindow(nullptr, PlayerWidget::BY_QMEDIA);
#endif
        playerWindow->setWindowTitle("Media Player");
        playerWindow->resize(800, 600);
        playerWindow->show();
        // this->hide();
    });
#endif

    // 添加轮盘组件按钮
    QPushButton *rouletteBtn = new QPushButton(tr("open roulette"));
    layout->addWidget(rouletteBtn);
    connect(rouletteBtn, &QPushButton::clicked, this, []() {
        // 创建轮盘对话框
        QDialog *rouletteDialog = new QDialog;
        rouletteDialog->setWindowTitle("Roulette");
        rouletteDialog->resize(400, 400);
        
        // 创建布局和轮盘组件
        QVBoxLayout *dialogLayout = new QVBoxLayout(rouletteDialog);
        SsJoystickWheel *roulette1 = new SsJoystickWheel;
        SsJoystickWheel *roulette2 = new SsJoystickWheel;
        dialogLayout->addWidget(roulette1);
        dialogLayout->addWidget(roulette2);
        
        // 设置为模态对话框
        rouletteDialog->setModal(true);
        rouletteDialog->exec();
        
        // 对话框关闭后自动删除
        rouletteDialog->deleteLater();
    });
}


#endif