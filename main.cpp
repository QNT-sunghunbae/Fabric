#include <QApplication>
#include <QStyleFactory>
#include <gst/gst.h>
#include "MainWindow.h"
#include "config.h" 

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    QApplication app(argc, argv);
    
    Config::load(); 

    qRegisterMetaType<Detection>("Detection");
    qRegisterMetaType<std::vector<Detection>>("std::vector<Detection>");
    qRegisterMetaType<DefectItem>("DefectItem");
    
    app.setStyle(QStyleFactory::create("Fusion"));
    
    MainWindow window;
    // window.showFullScreen(); 
    
    return app.exec();
}