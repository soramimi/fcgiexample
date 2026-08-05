TARGET = fcgiserv
TEMPLATE = app
CONFIG += console c++17
CONFIG -= app_bundle
CONFIG -= qt

OBJECTS_DIR = $$PWD/build/fcgiserv-obj

DESTDIR = $$PWD/_bin

unix:LIBS += -lpthread

SOURCES += \
	misc/ChaCha20.cpp \
	misc/uuid.cpp \
	serv/base64.cpp \
	serv/httpserver.cpp \
	serv/httpstatus.cpp \
	serv/mcp.cpp \
	serv/misc.cpp \
	serv/event.cpp \
	serv/FcgiProcess.cpp \
	serv/sha1.c \
	serv/debug.cpp \
	serv/main.cpp

HEADERS += \
	misc/ChaCha20.h \
	misc/fmt.h \
	misc/joinpath.h \
	misc/jstream.h \
	misc/strformat.h \
	misc/toi.h \
	misc/uuid.h \
	serv/base64.h \
	serv/httpserver.h \
	serv/httpstatus.h \
	serv/mcp.h \
	serv/misc.h \
	serv/sha1.h \
	serv/socket.h \
	serv/event.h \
	serv/mutex.h \
	serv/FcgiProcess.h \
	serv/debug.h
