QT       -= gui
CONFIG += c++11

TARGET = arnoldCamera
TEMPLATE = lib

DEFINES += ARNOLDCAMERA_LIBRARY

INCLUDEPATH += \
    /Volumes/ZENO_2016/projects/qt_zoic_old/Arnold-4.2.14.3-darwin/include \

LIBS += \
    -L/Volumes/ZENO_2016/projects/qt_zoic_old/Arnold-4.2.14.3-darwin/bin \

LIBS += \
    /Volumes/ZENO_2016/projects/qt_zoic_old/Arnold-4.2.14.3-darwin/bin/libai.dylib \

SOURCES += \
    zoic.cpp

HEADERS += \

DISTFILES += \
    ../ae/aiZoicTemplate.py \
    ../bin/zoic.mtd \
    buildflags.txt
