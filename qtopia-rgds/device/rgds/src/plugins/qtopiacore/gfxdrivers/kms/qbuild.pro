TEMPLATE=plugin
CONFIG+=qtopia
TARGET=kmsscreen

PLUGIN_FOR=qt
PLUGIN_TYPE=gfxdrivers

HEADERS += kmsscreendriverplugin.h kmsscreen.h
SOURCES += kmsscreendriverplugin.cpp kmsscreen.cpp

# libdrm headers live in <sysroot>/usr/include/libdrm
INCLUDEPATH+=/opt/rgds-sdk/sysroot/usr/include/libdrm
LIBS += -ldrm
