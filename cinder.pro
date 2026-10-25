QT += core gui widgets network
CONFIG += c++17
INCLUDEPATH += src
TARGET = cinder
TEMPLATE = app
RESOURCES += resources.qrc
QMAKE_CXXFLAGS += -Wall -Wextra -Wno-unused-parameter

# $ORIGIN/../lib is dist/lib when the binary lives in dist/bin. The Qt prefix
# is the fallback so ./cinder still runs from the build tree when Qt is installed.
unix:!macx {
    QMAKE_LFLAGS += -Wl,--disable-new-dtags
    QMAKE_LFLAGS += '-Wl,-rpath,\'\$$ORIGIN/../lib\''
    QMAKE_RPATHDIR += $$[QT_INSTALL_LIBS]
}

SOURCES += \
    src/main.cpp \
    src/common.cpp \
    src/settings.cpp \
    src/lexer.cpp \
    src/highlighter.cpp \
    src/editor.cpp \
    src/lint.cpp \
    src/runner.cpp \
    src/debugger.cpp \
    src/console.cpp \
    src/selftest.cpp \
    src/ui/mainwindow.cpp

HEADERS += \
    src/common.h \
    src/settings.h \
    src/lexer.h \
    src/highlighter.h \
    src/editor.h \
    src/lint.h \
    src/runner.h \
    src/debugger.h \
    src/console.h \
    src/selftest.h \
    src/ui/mainwindow.h
