# Makefile for ElevenLabs Conversational AI module
CC = gcc
CFLAGS = -fPIC -Wall -shared -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -g3 -O2 -D_REENTRANT -D_GNU_SOURCE -DAST_MODULE=\"app_elevenlabs_convai\"
ASTERISK_HEADERS = /usr/include/asterisk
LIBS = -lasterisk -lcurl -lcjson -lwebsockets -lssl -lcrypto -lpthread

TARGET = app_elevenlabs_convai.so
SOURCE = app_elevenlabs_convai.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -I$(ASTERISK_HEADERS) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/lib/asterisk/modules/

.PHONY: all clean install