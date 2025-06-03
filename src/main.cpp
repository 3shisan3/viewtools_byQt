#include <QApplication>
#include <QMainWindow>

#include "ability_manager.h"
#ifdef EXAMPLE_ON
#include "view/window/example/example.h"
#endif
#include "data/settings_manager.h"
#include "data/translation_manager.h"

void initModule()
{
    (void) SingletonTemplate<SsSettingsManager>::getSingletonInstance();
    (void) SingletonTemplate<SsTranslationManager>::getSingletonInstance();
}

int main(int argc, char *argv[])
{

    QApplication app(argc, argv);

    initModule();

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