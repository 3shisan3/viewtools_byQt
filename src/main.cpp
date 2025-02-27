#include <QApplication>
#include <QMainWindow>

#ifdef EXAMPLE_ON
#include "view/window/example/example.h"
#endif

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

#ifdef EXAMPLE_ON
    ExampleWindow window;
#else
    QMainWindow window;
    window.setWindowTitle("viewtools");
    window.setMinimumSize(800, 600);
#endif

    window.show();
    return app.exec();
}