BIN      := capslock-daemon
SRC      := src/capslock-daemon.c
LABEL    := com.tetsuroando.capslock-daemon
PLIST    := dist/$(LABEL).plist

PREFIX   ?= /usr/local
LIBEXEC  := $(PREFIX)/libexec
DAEMONS  := /Library/LaunchDaemons

CFLAGS   ?= -O2 -Wall -Wextra
LDFLAGS  := -framework IOKit -framework CoreFoundation

.PHONY: all clean install uninstall status log

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)

# Unload before replacing: the running daemon restores SleepDisabled=0 as it
# exits, so a straight overwrite could leave the machine unable to sleep.
install: $(BIN)
	@[ "$$(id -u)" -eq 0 ] || { echo "run: sudo make install" >&2; exit 1; }
	-launchctl bootout system/$(LABEL) 2>/dev/null
	install -d -m 755 -o root -g wheel $(LIBEXEC)
	install -m 755 -o root -g wheel $(BIN) $(LIBEXEC)/$(BIN)
	install -m 644 -o root -g wheel $(PLIST) $(DAEMONS)/$(LABEL).plist
	launchctl bootstrap system $(DAEMONS)/$(LABEL).plist
	@echo "installed. check with: make status"

uninstall:
	@[ "$$(id -u)" -eq 0 ] || { echo "run: sudo make uninstall" >&2; exit 1; }
	-launchctl bootout system/$(LABEL) 2>/dev/null
	rm -f $(DAEMONS)/$(LABEL).plist $(LIBEXEC)/$(BIN)
	@echo "uninstalled. SleepDisabled is now: $$(pmset -g | awk '/SleepDisabled/{print $$2}')"

status:
	@launchctl print system/$(LABEL) 2>/dev/null | grep -E '^\s+(state|pid|last exit)' || echo "not loaded"
	@pmset -g | grep -i sleepdisabled
	@tail -3 /var/log/capslock-daemon.log 2>/dev/null || true

log:
	@tail -f /var/log/capslock-daemon.log
