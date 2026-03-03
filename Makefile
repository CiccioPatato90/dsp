# Variables
CC = clang
CFLAGS = -Wall -std=c99
# Homebrew usually installs to /opt/homebrew on Apple Silicon
INCLUDE_PATH = -I/opt/homebrew/include
LIBRARY_PATH = -L/opt/homebrew/lib
LIBS = -lraylib -framework CoreVideo -framework IOKit -framework Cocoa -framework GLUT -framework OpenGL

# Target name
TARGET = app

SCRIPT = test.c circbuf.c

# Default rule
all: $(TARGET)

$(TARGET): $(SCRIPT)
	$(CC) $(CFLAGS) $(INCLUDE_PATH) $(LIBRARY_PATH) $(LIBS) $(SCRIPT) -o $(TARGET)

# Clean rule
clean:
	rm -f $(TARGET)

# Run rule
run: clean all
	./$(TARGET)
