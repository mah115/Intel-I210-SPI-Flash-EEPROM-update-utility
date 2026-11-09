CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

i210flash: i210flash.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f i210flash

.PHONY: clean
