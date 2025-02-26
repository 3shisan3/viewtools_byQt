#include <QApplication>
#include <QDebug>  
#include <QFile>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

void loadStyleSheetFromResource(const QString &path, QWidget *widget) {
    QFile styleFile(path);
    if (styleFile.open(QFile::ReadOnly)) {
        QString styleSheet = QLatin1String(styleFile.readAll());
        widget->setStyleSheet(styleSheet);  // 全局应用样式表
        styleFile.close();
    } else {
        qWarning() << "Failed to open style sheet resource:" << path;
    }
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("viewtools");
    window.setMinimumSize(800, 600);

    // 动态设置全局样式表
    loadStyleSheetFromResource(":/ui/style_sheet/example.qss", &window);

    // 添加一个按钮（test）
    QPushButton *button = new QPushButton("Click Me");
    QWidget *centralWidget = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(centralWidget);
    layout->addWidget(button);
    window.setCentralWidget(centralWidget);

    window.show();
    return app.exec();
}