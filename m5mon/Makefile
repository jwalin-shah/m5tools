TARGET = m5mon
INSTALL = $(HOME)/.local/bin/$(TARGET)

all: $(TARGET)

$(TARGET): main.go go.mod
	go build -o $(TARGET) .

install: $(TARGET)
	cp $(TARGET) $(INSTALL)
	codesign --sign - --force $(INSTALL)
	@echo "  [INSTALL] $(INSTALL)"

test: $(TARGET)
	go vet ./...
	@echo "  [TEST] go vet passed"
	@test -x $(TARGET)
	@echo "  [TEST] $(TARGET) binary exists and is executable"

clean:
	rm -f $(TARGET)

.PHONY: all install clean
