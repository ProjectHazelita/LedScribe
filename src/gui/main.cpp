#include "main_window.hpp"
#include "worker.hpp"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("LedScribe"));
    QCoreApplication::setOrganizationName(QStringLiteral("ProjectHazelita"));
    ledscribe::gui::registerEditorMetaTypes();
    ledscribe::gui::MainWindow window;
    window.show();
    return application.exec();
}
