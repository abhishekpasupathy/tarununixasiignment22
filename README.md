# Signal-Triggered Checkpoint and Resume for Batch Computation

A POSIX C batch program that periodically saves deterministic computation progress and responds to `SIGUSR1` by requesting an early checkpoint. Signal handlers only set `volatile sig_atomic_t` flags. All checkpoint I/O is deferred to the main execution loop.

## Requirements

- macOS or Linux
- C compiler (`clang` or `gcc`)
- `make`

## Build and run

```bash
git clone https://github.com/abhishekpasupathy/tarununixasiignment22.git
cd tarununixasiignment22
make
./checkpoint_batch
```

Arguments: `./checkpoint_batch TOTAL_STEPS CHECKPOINT_INTERVAL WORK_DELAY_MS`

Example: `./checkpoint_batch 500000 50000 1`

The program prints its PID. In a second terminal, request an early checkpoint with `kill -USR1 PID`. Start the same command again to resume from the last committed checkpoint. Use the same TOTAL_STEPS value.

## Checkpoint and recovery design

- `sigaction(SIGUSR1, ...)` requests an early checkpoint.
- The handler performs only an assignment to `volatile sig_atomic_t`; no file I/O, allocation, printing, or unsafe library functions.
- The main loop checks and clears the request with SIGUSR1 briefly blocked using `sigprocmask`.
- Checkpoint records contain a magic value, version, total steps, completed steps, accumulator and checksum.
- Write to `batch.checkpoint.tmp`, `fsync` the complete data, close it, then atomically `rename` to `batch.checkpoint`. The containing directory is also synced.
- A leftover temporary file is never considered committed state; an earlier committed checkpoint remains recoverable if a write is interrupted before rename.
- Startup validates record length, magic, version, requested total, progress bounds, and checksum before resuming.
- Invalid or incompatible checkpoint files are rejected and computation restarts from step zero.
- SIGINT and SIGTERM request the main loop to save progress and exit gracefully.

## Tests

```bash
make test
```

Integration tests cover periodic checkpoints, SIGUSR1, repeated signals, an orphaned/truncated temporary write, and invalid checkpoint recovery.

## Files

- `checkpoint_batch.c` — implementation
- `test_checkpoint.sh` — integration tests
- `Makefile` — build and test commands

Only send signals to processes you own or are authorized to control.
