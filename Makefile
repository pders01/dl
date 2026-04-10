CC     ?= cc
CFLAGS ?= -Wall -Wextra -Wpedantic -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

dl: dl.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f dl

install: dl
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 dl $(DESTDIR)$(BINDIR)/dl

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/dl

.PHONY: clean install uninstall
