CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS += -lasound

.PHONY: all check clean
all: qx18

qx18: qx18.o wav.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

qx18.o: qx18.c wav.h
wav.o: wav.c wav.h

test_wav: test_wav.c wav.c wav.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ test_wav.c wav.c

check: qx18 test_wav
	./test_wav
	./qx18 --help >/dev/null
	@if ./qx18 --max-seconds 0 >/dev/null 2>&1; then exit 1; fi

clean:
	rm -f qx18 test_wav *.o
