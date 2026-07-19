CC      = cc
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -MP -MD
TARGET  = m5fand
INSTALL = $(HOME)/.local/bin/$(TARGET)

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -O2 -o $(TARGET) main.c
	@echo "  [BUILD] $(TARGET)"

-include *.d

install: $(TARGET)
	cp $(TARGET) $(INSTALL)
	codesign --sign - --force $(INSTALL)
	@echo "  [INSTALL] $(INSTALL)"

test: $(TARGET)
	@echo "  [TEST] $(TARGET) binary exists and is executable"
	@test -x $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install test clean
