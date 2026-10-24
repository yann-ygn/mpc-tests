#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Read-only process-memory research helper. No ptrace attachment, writes, or calls
 * into the target. Scan prints addresses only; read returns bounded binary data. */
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: mem_probe PID scan HEX... | PID read HEX LENGTH\n"); return 2; }
    char path[80];
    snprintf(path, sizeof path, "/proc/%ld/mem", strtol(argv[1], NULL, 10));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror("open memory"); return 1; }
    if (!strcmp(argv[2], "read")) {
        if (argc != 5) return 2;
        uint64_t address = strtoull(argv[3], NULL, 16);
        size_t length = strtoul(argv[4], NULL, 10);
        if (!length || length > 8 * 1024 * 1024) return 2;
        unsigned char buf[65536];
        while (length) {
            size_t want = length < sizeof buf ? length : sizeof buf;
            ssize_t n = pread(fd, buf, want, (off_t)address);
            if (n <= 0) { perror("read memory"); close(fd); return 1; }
            if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) return 1;
            address += (size_t)n;
            length -= (size_t)n;
        }
        close(fd);
        return 0;
    }
    if (strcmp(argv[2], "scan") || argc > 35) return 2;
    uint32_t patterns[32];
    for (int j = 3; j < argc; j++) patterns[j - 3] = (uint32_t)strtoul(argv[j], NULL, 16);
    snprintf(path, sizeof path, "/proc/%ld/maps", strtol(argv[1], NULL, 10));
    FILE *maps = fopen(path, "r");
    if (!maps) { perror("open maps"); close(fd); return 1; }
    char line[1024]; uint32_t buf[16384];
    while (fgets(line, sizeof line, maps)) {
        if (!strstr(line, "[heap]")) continue;
        unsigned long long start, end;
        if (sscanf(line, "%llx-%llx", &start, &end) != 2) return 1;
        for (uint64_t off = start; off < end;) {
            size_t size = end - off < sizeof buf ? (size_t)(end - off) : sizeof buf;
            ssize_t got = pread(fd, buf, size, (off_t)off);
            if (got <= 0) { perror("scan memory"); fclose(maps); close(fd); return 1; }
            for (size_t i = 0; i < (size_t)got / 4; i++)
                for (int j = 0; j < argc - 3; j++) if (buf[i] == patterns[j])
                    printf("%08" PRIx64 " %08" PRIx32 "\n", off + i * 4, patterns[j]);
            off += (size_t)got;
        }
    }
    fclose(maps); close(fd);
    return 0;
}
