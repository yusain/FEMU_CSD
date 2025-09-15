// build: gcc -O2 -Wall -Wextra -Wshadow $(pkg-config --cflags openssl 2>/dev/null) checksum_test.c -o EXE/checksum_test.exe $(pkg-config --libs openssl 2>/dev/null || echo -lcrypto)
// run  : sudo ./EXE/checksum_test.exe

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <string.h>
#include <time.h>
#include <openssl/sha.h>

/* 參照 filter_* 測試格式：
 * - 三種顆粒（總資料量）：10MiB、1MiB、4KiB
 * - 每次 I/O 單筆傳輸量（chunk）不得大於 512KiB（此處採 384KiB）
 * - 不需 sh；單一執行檔依序完成三組測試
 *
 * 比對說明：
 * device result = 韌體對「對齊後 data_len」做 SHA-256，取前 4 bytes 以 little-endian 當成 uint32 回傳
 * host   result = 主機端對「同一份 buffer 的 data_len（含尾端補 0）」做 SHA-256，取前 4 bytes 以 little-endian 組成 uint32
 * 判斷：相等則 [MATCH]，否則 [MISMATCH]
 */

#define NVME_DEVICE "/dev/nvme0n1"
#define OPCODE_CSD_CHECKSUM 0xC1
#define OPCODE_CSD_REPORT   0xCF
#define OPCODE_CSD_RESET    0xCE

/* ---- 常數與工具 ---- */
#define KiB(x) ((size_t)((x) * 1024ULL))
#define MiB(x) ((size_t)((x) * 1024ULL * 1024ULL))

#define SECTOR     512u
#define ALIGN4096  4096u
#define CHUNK_MAX  (384u * 1024u)   /* 384 KiB；< 512 KiB */
#define Logflag    1                /* 1=列印 SHA 與比對；0=只印總結 */

static inline size_t rup(size_t x, size_t a){ return (x + a - 1) / a * a; }

/* 以 hex 字串輸出 SHA-256（供保留「原本」那行 SHA256(host)=... 使用） */
static void sha256_hex(const unsigned char *buf, size_t len, char out_hex[65]){
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256(buf, len, d);
    static const char* hexd = "0123456789abcdef";
    for (int i=0;i<SHA256_DIGEST_LENGTH;++i){
        out_hex[i*2+0] = hexd[(d[i] >> 4) & 0xF];
        out_hex[i*2+1] = hexd[(d[i]     ) & 0xF];
    }
    out_hex[64] = '\0';
}

/* 取 SHA256 的前 4 bytes，並以 little-endian 組成 uint32_t（與裝置端對齊） */
static uint32_t sha256_first4_le_u32(const unsigned char *buf, size_t len){
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256(buf, len, d);
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

/* 執行一個顆粒（總資料量 total_bytes）；內部分割為多筆 <= CHUNK_MAX 的 I/O */
static void run_one_case(int fd, FILE *dat, const char *label, size_t total_bytes){
    void *buf = aligned_alloc(ALIGN4096, rup(CHUNK_MAX, ALIGN4096));
    if (!buf){ perror("aligned_alloc"); return; }

    setvbuf(dat, NULL, _IOFBF, 1<<20);  /* 降低 fread 開銷（可選） */

    uint64_t ops = 0, sent = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (sent < total_bytes){
        size_t want = total_bytes - sent;
        if (want > CHUNK_MAX) want = CHUNK_MAX;

        size_t rd = fread(buf, 1, want, dat);   /* raw bytes */
        if (rd == 0){ if (ferror(dat)) perror("fread"); break; }

        /* 對齊：先 512B（LBA），再 4096B（DMA）；尾端補 0 以避免髒資料 */
        size_t data_len = rup(rup(rd ? rd : SECTOR, SECTOR), ALIGN4096);
        if (data_len > rd) memset((uint8_t*)buf + rd, 0, data_len - rd);

        #if Logflag
        {
            /* 你要保留的「原本」那行：針對 rd（未對齊）的 SHA */
            char hex_raw[65];
            sha256_hex((unsigned char*)buf, rd, hex_raw);
            printf("SHA256(host) = %s | ", hex_raw);
        }
        #endif

        struct nvme_passthru_cmd cmd = {
            .opcode   = OPCODE_CSD_CHECKSUM,
            .nsid     = 1,
            .addr     = (uintptr_t)buf,
            .data_len = (uint32_t)data_len,
            .cdw10    = (uint32_t)rd,                    /* 備註：目前韌體未使用 */
            .cdw12    = (uint32_t)(data_len/SECTOR - 1), /* NLB-1 */
        };

        if (ioctl(fd, NVME_IOCTL_IO_CMD, &cmd) < 0){
            perror("ioctl checksum");
            break;
        }

        #if Logflag
        {
            /* 與裝置回傳比對：必須用 data_len（含補零後對齊長度） */
            uint32_t host32 = sha256_first4_le_u32((unsigned char*)buf, data_len);
            printf("device result=0x%08x | host result=0x%08x %s",
                   cmd.result, host32, (cmd.result == host32) ? "[MATCH]" : "[MISMATCH]");

            /* 若有對齊補零，額外印出「對齊後」SHA 便於除錯 */
            if (data_len != rd){
                char hex_aln[65];
                sha256_hex((unsigned char*)buf, data_len, hex_aln);
                printf(" | SHA256(host, aligned=%zu) = %s", data_len, hex_aln);
            }
            printf("\n");
        }
        #endif

        sent += rd;
        ops++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t total_us = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000ULL +
                        (uint64_t)(t1.tv_nsec - t0.tv_nsec) / 1000ULL;

    /* QEMU/FEMU 端會印裝置端彙總 */
    struct nvme_passthru_cmd report = { .opcode = OPCODE_CSD_REPORT, .nsid = 1 };
    if (ioctl(fd, NVME_IOCTL_IO_CMD, &report) < 0){
        fprintf(stderr, "ioctl REPORT error (%d): %s\n", errno, strerror(errno));
    }

    double avg_op_us = ops ? (double)total_us / (double)ops : 0.0;
    double bw_mib_s  = total_us ? (((double)sent / 1048576.0) / ((double)total_us / 1e6)) : 0.0;
    double total_mib = (double)sent / 1048576.0;

    /* filter_* 風格的摘要行 */
    printf("CHECKSUM %-10s Ops=%-3llu | Bytes=%6.3f MiB | Total=%8llu us | Avg/op:%6.0f us | Avg BW:%7.2f MiB/s\n",
           label, (unsigned long long)ops, total_mib,
           (unsigned long long)total_us, avg_op_us, bw_mib_s);

    free(buf);

    /* 重置裝置端統計，準備下一顆粒 */
    struct nvme_passthru_cmd reset = { .opcode = OPCODE_CSD_RESET, .nsid = 1 };
    if (ioctl(fd, NVME_IOCTL_IO_CMD, &reset) < 0){
        fprintf(stderr, "ioctl RESET error (%d): %s\n", errno, strerror(errno));
    }
}

int main(void){
    int fd = open(NVME_DEVICE, O_RDWR);
    if (fd < 0){ perror("open nvme device"); return 1; }

    /* 資料來源：/dev/urandom（避免 /dev/random 阻塞）；也可改 /dev/zero 以利重現 */
    // FILE *dat = fopen("/dev/random", "rb");   // 可能阻塞
    FILE *dat = fopen("/dev/urandom", "rb");     // 建議
    // FILE *dat = fopen("/dev/zero", "rb");     // 可重現性
    if (!dat){ perror("open data source"); close(fd); return 1; }

    /* 三顆粒；內部自動切分成多筆（<= CHUNK_MAX） */
    run_one_case(fd, dat, "URND_10M", MiB(10));
    run_one_case(fd, dat, "URND_1M",  MiB(1));
    run_one_case(fd, dat, "URND_4K",  KiB(4));

    fclose(dat); close(fd); return 0;
}
