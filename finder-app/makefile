# Compiler and flags
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror

# Target executable
TARGET = writer

# Source files
SRCS = writer.c
OBJS = $(SRCS:.c=.o)

# Default target: build the writer application
all: $(TARGET)

# Build the writer executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# Compile source files to object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target: remove the application and object files
clean:
	rm -f $(TARGET) $(OBJS)

# Phony targets
.PHONY: all clean
