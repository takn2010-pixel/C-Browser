cat << 'EOF' > Makefile
CC = gcc
CFLAGS = `pkg-config --cflags gtk+-3.0 libcurl gumbo libcss libwapcaplet`
LIBS = `pkg-config --libs gtk+-3.0 libcurl gumbo libcss libwapcaplet`
TARGET = c-browser
SRC = browser.c

all: \$(TARGET)

\$(TARGET): \((SRC)\)(CC) -o \((TARGET)\)(SRC) \((CFLAGS)\)(LIBS)

clean:
	rm -f \$(TARGET)
EOF
