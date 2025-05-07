# Simple Makefile for GTK4 Simulator

# Compiler and flags
CC = gcc

# Get GTK flags using pkg-config
GTK_CFLAGS = $(shell pkg-config --cflags gtk4)
GTK_LIBS = $(shell pkg-config --libs gtk4)

# General flags
CFLAGS = -Wall -Wextra -g $(GTK_CFLAGS)
LIBS = $(GTK_LIBS) -lm # Add -lm if simulator uses math functions

# Source files
SRCS = gui.c simulator.c

# Object files
OBJS = $(SRCS:.c=.o)

# Executable name
TARGET = minisimgui

# Default target
all: $(TARGET)

# Link target
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LIBS)

# Compile source files to object files
%.o: %.c simulator.h
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
clean:
	rm -f $(OBJS) $(TARGET)

# Phony targets
.PHONY: all clean 