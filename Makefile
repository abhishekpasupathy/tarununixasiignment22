CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
.PHONY: all clean test
all: checkpoint_batch
checkpoint_batch: checkpoint_batch.c
	$(CC) $(CFLAGS) -o $@ $<
test: checkpoint_batch
	bash ./test_checkpoint.sh
clean:
	rm -f checkpoint_batch
