/*
 * Signal-Triggered Checkpoint and Resume for Batch Computation
 * Signal handlers only set sig_atomic_t flags; main-loop code performs I/O.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECKPOINT_FILE "batch.checkpoint"
#define TEMP_FILE "batch.checkpoint.tmp"
#define MAGIC "BATCHCP1"
#define MAX_STEPS UINT64_C(1000000000)

static volatile sig_atomic_t checkpoint_requested;
static volatile sig_atomic_t stop_requested;

typedef struct {
    char magic[8];
    uint64_t version, total_steps, completed_steps, accumulator, checksum;
} checkpoint_t;

static void on_checkpoint(int sig) { (void)sig; checkpoint_requested = 1; }
static void on_stop(int sig) { (void)sig; stop_requested = 1; }

static uint64_t hash_bytes(const void *data, size_t size) {
    const unsigned char *p = data;
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}
static uint64_t checksum(const checkpoint_t *cp) {
    return hash_bytes(cp, offsetof(checkpoint_t, checksum));
}
static uint64_t compute_step(uint64_t value, uint64_t step) {
    value ^= step + UINT64_C(0x9e3779b97f4a7c15);
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 29;
    return value;
}
static int pause_ms(unsigned long ms) {
    struct timespec req = {(time_t)(ms / 1000UL), (long)(ms % 1000UL) * 1000000L};
    while (nanosleep(&req, &req) < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    return 0;
}
static int write_all(int fd, const void *data, size_t size) {
    const unsigned char *p = data;
    size_t off = 0;
    while (off < size) {
        ssize_t n = write(fd, p + off, size - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = EIO; return -1; }
        off += (size_t)n;
    }
    return 0;
}
static int save_checkpoint(uint64_t total, uint64_t done, uint64_t value) {
    checkpoint_t cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.magic, MAGIC, sizeof(cp.magic));
    cp.version = 1; cp.total_steps = total; cp.completed_steps = done;
    cp.accumulator = value; cp.checksum = checksum(&cp);

    int fd = open(TEMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open temp checkpoint"); return -1; }
    int rc = 0;
    if (write_all(fd, &cp, sizeof(cp)) < 0 || fsync(fd) < 0) {
        perror("write/fsync checkpoint"); rc = -1;
    }
    if (close(fd) < 0) { perror("close checkpoint"); rc = -1; }
    if (rc < 0) { unlink(TEMP_FILE); return -1; }
    if (rename(TEMP_FILE, CHECKPOINT_FILE) < 0) {
        perror("atomic rename"); unlink(TEMP_FILE); return -1;
    }
    int dirfd = open(".", O_RDONLY);
    if (dirfd >= 0) {
        if (fsync(dirfd) < 0) { perror("fsync directory"); close(dirfd); return -1; }
        close(dirfd);
    }
    printf("[checkpoint] committed step %" PRIu64 "/%" PRIu64 "\n", done, total);
    fflush(stdout);
    return 0;
}
static int load_checkpoint(uint64_t total, uint64_t *done, uint64_t *value) {
    int fd = open(CHECKPOINT_FILE, O_RDONLY);
    if (fd < 0) { if (errno == ENOENT) return 0; perror("open checkpoint"); return -1; }
    checkpoint_t cp;
    size_t off = 0;
    while (off < sizeof(cp)) {
        ssize_t n = read(fd, (unsigned char *)&cp + off, sizeof(cp) - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { perror("read checkpoint"); close(fd); return -1; }
        if (n == 0) break;
        off += (size_t)n;
    }
    unsigned char extra;
    ssize_t more;
    do { more = read(fd, &extra, 1); } while (more < 0 && errno == EINTR);
    close(fd);
    if (off != sizeof(cp) || more != 0 ||
        memcmp(cp.magic, MAGIC, sizeof(cp.magic)) != 0 ||
        cp.version != 1 || cp.total_steps != total ||
        cp.completed_steps > cp.total_steps || cp.checksum != checksum(&cp)) {
        fprintf(stderr, "[recovery] invalid/incompatible checkpoint; restarting at step 0\n");
        return 0;
    }
    *done = cp.completed_steps; *value = cp.accumulator;
    printf("[recovery] valid checkpoint: resuming at step %" PRIu64 "\n", *done);
    return 1;
}
static int install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_checkpoint;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) { perror("sigaction SIGUSR1"); return -1; }
    sa.sa_handler = on_stop;
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction stop"); return -1;
    }
    return 0;
}
int main(int argc, char **argv) {
    uint64_t total = UINT64_C(1000000), interval = UINT64_C(100000);
    unsigned long delay = 1;
    if (argc > 4) {
        fprintf(stderr, "Usage: %s [TOTAL_STEPS] [CHECKPOINT_INTERVAL] [WORK_DELAY_MS]\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *end = NULL;
    if (argc > 1) { errno = 0; total = strtoull(argv[1], &end, 10); if (errno || !end || *end) return 2; }
    if (argc > 2) { errno = 0; interval = strtoull(argv[2], &end, 10); if (errno || !end || *end) return 2; }
    if (argc > 3) { errno = 0; delay = strtoul(argv[3], &end, 10); if (errno || !end || *end) return 2; }
    if (total == 0 || total > MAX_STEPS || interval == 0 || delay > 60000UL) {
        fprintf(stderr, "Invalid arguments: steps 1..%" PRIu64 ", interval > 0, delay <= 60000 ms\n", MAX_STEPS);
        return EXIT_FAILURE;
    }
    if (install_handlers() < 0) return EXIT_FAILURE;

    uint64_t done = 0, value = 0;
    if (load_checkpoint(total, &done, &value) < 0) return EXIT_FAILURE;
    printf("[batch] PID=%ld total=%" PRIu64 " interval=%" PRIu64 " delay=%lu ms\n",
           (long)getpid(), total, interval, delay);
    printf("[batch] Send SIGUSR1 to request an early checkpoint.\n");
    uint64_t last_checkpoint = done;

    while (done < total) {
        value = compute_step(value, done + 1);
        ++done;
        if (delay && pause_ms(delay) < 0) { perror("nanosleep"); return EXIT_FAILURE; }

        if (checkpoint_requested) {
            sigset_t set, old;
            sigemptyset(&set); sigaddset(&set, SIGUSR1);
            if (sigprocmask(SIG_BLOCK, &set, &old) < 0) { perror("sigprocmask"); return EXIT_FAILURE; }
            checkpoint_requested = 0;
            if (sigprocmask(SIG_SETMASK, &old, NULL) < 0) { perror("restore signal mask"); return EXIT_FAILURE; }
            if (save_checkpoint(total, done, value) < 0) return EXIT_FAILURE;
            last_checkpoint = done;
        }
        if (done - last_checkpoint >= interval) {
            if (save_checkpoint(total, done, value) < 0) return EXIT_FAILURE;
            last_checkpoint = done;
        }
        if (stop_requested) {
            printf("[batch] stop requested; saving progress before exit\n");
            if (save_checkpoint(total, done, value) < 0) return EXIT_FAILURE;
            return 128 + SIGTERM;
        }
    }
    if (last_checkpoint != done && save_checkpoint(total, done, value) < 0) return EXIT_FAILURE;
    printf("[batch] COMPLETE steps=%" PRIu64 " result=%" PRIu64 "\n", done, value);
    return EXIT_SUCCESS;
}
