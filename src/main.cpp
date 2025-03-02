#include <QApplication>
#include <QMainWindow>

#include "ability_manager.h"
#ifdef EXAMPLE_ON
#include "view/window/example/example.h"
#endif

void initModule()
{
    
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

#ifdef EXAMPLE_ON
    ExampleWindow window;
#else
    QMainWindow window;
    window.setWindowTitle(QObject::tr("viewtools"));
    window.setMinimumSize(800, 600);
#endif

    window.show();
    return app.exec();
}