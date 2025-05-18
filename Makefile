CC = gcc

GTK_CFLAGS = $(shell pkg-config --cflags gtk4)
GTK_LIBS = $(shell pkg-config --libs gtk4)

CFLAGS = -Wall -Wextra -g $(GTK_CFLAGS)
LIBS = $(GTK_LIBS) -lm

SRCS = gui.c simulator.c

OBJS = $(SRCS:.c=.o)

TARGET = minisimgui

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LIBS)

%.o: %.c simulator.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean 