CFLAGS ?= -O2 -Wall -Wextra

SVN_CFLAGS ?= $(shell pkg-config --cflags libsvn_client)
SVN_LIBS ?= $(shell pkg-config --libs libsvn_client) -lsvn_subr-1

all: build/orthos

build/orthos: orthos.c orthos.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(SVN_CFLAGS) $(CFLAGS) -std=c11 $(LDFLAGS) -o $@ orthos.c $(SVN_LIBS) $(LDLIBS)

check: build/orthos
	python3 tests/test_watcher.py -v

.PHONY: all check
