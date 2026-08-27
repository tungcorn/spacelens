#include "TestRunner.hpp"
#include "ui/UiTheme.hpp"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    spacelens::applyApplicationChrome(app);
    return spacelens::test::runAll();
}
