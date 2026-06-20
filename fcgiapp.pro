TARGET = fcgiapp
TEMPLATE = app
CONFIG += console c++17
CONFIG -= app_bundle
CONFIG -= qt

OBJECTS_DIR = $$PWD/build/fcgiapp-obj

INCLUDEPATH += $$PWD/fcgi/include

DESTDIR = $$PWD/_bin

SOURCES += \
	fcgi/libfcgi/fcgi_stdio.c \
	fcgi/libfcgi/fcgiapp.c \
	app/myfcgi.c \
	app/main.cpp \
	serv/debug.cpp

win32:SOURCES += fcgi/libfcgi/os_win32.c
unix:SOURCES += fcgi/libfcgi/os_unix.c

HEADERS += \
	fcgi/include/fastcgi.h \
	fcgi/include/fcgi_config_x86.h \
	fcgi/include/fcgi_stdio.h \
	fcgi/include/fcgiapp.h \
	fcgi/include/fcgimisc.h \
	fcgi/include/fcgio.h \
	fcgi/include/fcgios.h \
	app/myfcgi.h \
	serv/debug.h

