CC      = /usr/bin/clang
CFLAGS  = -std=c99 -Wall -Wextra -pedantic
TARGET  = m5fand
INSTALL = $(HOME)/.local/bin/$(TARGET)

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -O2 -o $(TARGET) main.c
	@echo "  [BUILD] $(TARGET)"

install: $(TARGET)
	cp $(TARGET) $(INSTALL)
	@echo "  [INSTALL] $(INSTALL)"

clean:
	rm -f $(TARGET)

.PHONY: all install clean
