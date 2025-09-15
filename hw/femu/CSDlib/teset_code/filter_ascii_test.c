// build: gcc -O2 -Wall filter_ascii_test.c -o EXE/filter_ascii_test
// run  : sudo ./EXE/filter_ascii_test <input> <output>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/nvme_ioctl.h>

#define NVME_DEVICE "/dev/nvme0n1"
#define OPCODE_CSD  0xC0
#define MODE_ASCII  2
#define CHUNK_MAX   (384*1024)   // < 512 KiB
#define SECTOR      512
#define ALIGN4096   4096
#define WRAP_COLS   64
static size_t rup(size_t n, size_t a){ return (n + a - 1)/a*a; }
static inline uint64_t usdiff(struct timespec a, struct timespec b){
    time_t s = b.tv_sec - a.tv_sec; long ns = b.tv_nsec - a.tv_nsec;
    if (ns < 0){ ns += 1000000000L; s -= 1; }
    return (uint64_t)s*1000000ULL + (uint64_t)ns/1000ULL;
}

int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr,"Usage: %s <input> <output>\n", argv[0]); return 1; }

    int fd = open(NVME_DEVICE, O_RDWR); if (fd < 0){ perror("open nvme"); return 1; }
    FILE *fi = fopen(argv[1], "rb");    if (!fi){ perror("fopen input"); close(fd); return 1; }
    FILE *fo = fopen(argv[2], "wb");    if (!fo){ perror("fopen output"); fclose(fi); close(fd); return 1; }

    void *buf = aligned_alloc(ALIGN4096, rup(CHUNK_MAX, ALIGN4096));
    if (!buf){ perror("aligned_alloc"); fclose(fo); fclose(fi); close(fd); return 1; }

    uint64_t in_bytes=0, out_bytes=0, ops=0, total_us=0; int col=0;

    for(;;){
        size_t rd = fread(buf, 1, CHUNK_MAX, fi);
        if (rd == 0) break; in_bytes += rd;

        size_t data_len = rup(rup(rd ? rd : SECTOR, SECTOR), ALIGN4096);
        struct nvme_passthru_cmd cmd = {
            .opcode   = OPCODE_CSD,
            .nsid     = 1,
            .addr     = (uintptr_t)buf,
            .data_len = (uint32_t)data_len,
            .cdw10    = (uint32_t)rd,
            .cdw12    = (uint32_t)(data_len/SECTOR - 1),
            .cdw15    = MODE_ASCII,
        };

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (ioctl(fd, NVME_IOCTL_IO_CMD, &cmd) < 0){ perror("ioctl"); break; }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        size_t kept = (size_t)cmd.result;
        for (size_t i=0;i<kept;i++){
            fputc(((uint8_t*)buf)[i], fo);
            if (++col == WRAP_COLS){ fputc('\n', fo); col = 0; }
        }
        out_bytes += kept;
        total_us  += usdiff(t0,t1);
        ops++;
    }
    if (col) fputc('\n', fo);

    double mib_in  = in_bytes  / (1024.0*1024.0);
    double mib_out = out_bytes / (1024.0*1024.0);
    double sec     = total_us / 1e6;
    double bw      = sec>0 ? (mib_in/sec) : 0.0;
    double avg     = ops? ((double)total_us/ops) : 0.0;

printf("Ops=%3llu | Bytes=%6.3f MiB | Out=%6.3f MiB | Total=%8llu us | Avg/op:%5.0f us | Avg BW:%8.2f MiB/s -> %s\n",
       (unsigned long long)ops, mib_in, mib_out, (unsigned long long)total_us,
       avg, bw, argv[2] /* 你的輸出檔變數名 */);


    free(buf); fclose(fo); fclose(fi); close(fd); return 0;
}
