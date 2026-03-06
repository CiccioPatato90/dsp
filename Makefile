# Variables
CC = clang
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=199309L
# Homebrew usually installs to /opt/homebrew on Apple Silicon
# INCLUDE_PATH = -I/opt/homebrew/include
# LIBRARY_PATH = -L/opt/homebrew/lib
INCLUDE_PATH = -I/usr/include
LIBRARY_PATH = -L/usr/lib64
LIBS = -lraylib -lm -lpthread
MACOS_FRAMEWORK = -framework CoreVideo -framework IOKit -framework Cocoa -framework GLUT -framework OpenGL
OPTIMIZATION = -O3

# Target name
TARGET = main

SCRIPT = main.c lib/circbuf.c

# Default rule
all: $(TARGET)

$(TARGET): $(SCRIPT)
	$(CC) $(OPTIMIZATION) $(CFLAGS) $(INCLUDE_PATH) $(LIBRARY_PATH) $(LIBS) $(SCRIPT) -o build/$(TARGET)

# Clean rule
clean:
	rm -f build/$(TARGET)

# Run rule
run: clean all
	./build/$(TARGET)
