TARGET = fcgiserv
TEMPLATE = app
CONFIG += console c++11
CONFIG -= app_bundle
CONFIG -= qt

DESTDIR = $$PWD/_bin

unix:LIBS += -lpthread

SOURCES += \
    serv/httpserver.cpp \
    serv/httpstatus.cpp \
    serv/misc.cpp \
    serv/event.cpp \
    serv/FcgiProcess.cpp \
    serv/thread.cpp \
	serv/debug.cpp \
    serv/joinpath.cpp \
    serv/main.cpp

HEADERS += \
    serv/httpserver.h \
    serv/httpstatus.h \
    serv/misc.h \
    serv/socket.h \
    serv/event.h \
    serv/mutex.h \
    serv/FcgiProcess.h \
    serv/thread.h \
	serv/debug.h \
    serv/joinpath.h
