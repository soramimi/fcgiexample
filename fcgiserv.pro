TARGET = fcgiserv
TEMPLATE = app
CONFIG += console c++11
CONFIG -= app_bundle
CONFIG -= qt

DESTDIR = $$PWD/_bin

unix:LIBS += -lpthread

SOURCES += \
    serv/base64.cpp \
    serv/httpserver.cpp \
    serv/httpstatus.cpp \
    serv/misc.cpp \
    serv/event.cpp \
    serv/FcgiProcess.cpp \
    serv/sha1.c \
    serv/thread.cpp \
	serv/debug.cpp \
    serv/joinpath.cpp \
    serv/main.cpp

HEADERS += \
    serv/base64.h \
    serv/httpserver.h \
    serv/httpstatus.h \
    serv/misc.h \
    serv/sha1.h \
    serv/socket.h \
    serv/event.h \
    serv/mutex.h \
    serv/FcgiProcess.h \
	serv/strformat.h \
    serv/thread.h \
	serv/debug.h \
    serv/joinpath.h
