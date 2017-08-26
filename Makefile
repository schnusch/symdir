cflags = -std=c99 -O2 -Wall -Wextra -Wpedantic -Wshadow \
		-Werror=implicit-function-declaration -Werror=vla \
		$(CFLAGS)
ldflags = $(LDFLAGS)

PREFIX  ?= /usr/local
DESTDIR ?=

INSTALL ?= install
RST2MAN ?= rst2man.py

all:   build doc
build: symdir
doc:   symdir.1
clean:
	$(RM) symdir symdir.1
install: all
	$(INSTALL) -D     symdir   $(DESTDIR)$(PREFIX)/bin/symdir
	$(INSTALL) -Dm644 symdir.1 $(DESTDIR)$(PREFIX)/share/man/man1/symdir.1

symdir: symdir.c
	$(strip $(CC) $(cflags) -o $@ $^ $(ldflags))

symdir.1: man.rst
	$(RST2MAN) $< $@
