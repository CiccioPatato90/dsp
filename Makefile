# Variables
CC = clang
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=199309L
LIBS = -lraylib -lm -lpthread
OPTIMIZATION = -O3

# Target name
TARGET = main
SCRIPT = main.c lib/circbuf.c

# Detect OS
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Darwin)
	INCLUDE_PATH = -I/opt/homebrew/include
	LIBRARY_PATH = -L/opt/homebrew/lib
	MACOS_FRAMEWORK = -framework CoreVideo -framework IOKit -framework Cocoa -framework GLUT -framework OpenGL
else
	INCLUDE_PATH = -I/usr/include
	LIBRARY_PATH = -L/usr/lib64
	MACOS_FRAMEWORK =
endif

# Default rule
all: $(TARGET)

$(TARGET): $(SCRIPT)
	$(CC) $(OPTIMIZATION) $(CFLAGS) $(INCLUDE_PATH) $(LIBRARY_PATH) $(MACOS_FRAMEWORK) $(LIBS) $(SCRIPT) -o build/$(TARGET)

# Clean rule
clean:
	rm -f build/$(TARGET)

# Run rule
run: clean all
	./build/$(TARGET)
