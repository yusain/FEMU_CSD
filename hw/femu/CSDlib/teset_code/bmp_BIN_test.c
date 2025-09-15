// bmp_BIN_test.c — Route B：黑白二值 + 主/裝置雙向驗證（少量輸出）
// build: gcc -O2 -Wall bmp_BIN_test.c -o bmp_BIN.exe
// run  : sudo ./EXE/bmp_BIN.exe

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>

/*
 The nvme_passthru_cmd structure is defined in the provided header file (nvme_ioctl.h) and is used to 
represent a command that can be passed through to an NVMe device. It allows users to send custom 
commands directly to the NVMe controller, bypassing the standard kernel I/O stack.

Structure Definition:

struct nvme_passthru_cmd {
    __u8	opcode;         // Command opcode
    __u8	flags;          // Command flags
    __u16	rsvd1;          // Reserved
    __u32	nsid;           // Namespace identifier
    __u32	cdw2;           // Command-specific DWORD 2
    __u32	cdw3;           // Command-specific DWORD 3
    __u64	metadata;       // Metadata buffer address
    __u64	addr;           // Data buffer address
    __u32	metadata_len;   // Metadata buffer length
    __u32	data_len;       // Data buffer length
    __u32	cdw10;          // Command-specific DWORD 10
    __u32	cdw11;          // Command-specific DWORD 11
    __u32	cdw12;          // Command-specific DWORD 12
    __u32	cdw13;          // Command-specific DWORD 13
    __u32	cdw14;          // Command-specific DWORD 14
    __u32	cdw15;          // Command-specific DWORD 15
    __u32	timeout_ms;     // Command timeout in milliseconds
    __u32	result;         // Command result
};

Key Fields:
opcode: Specifies the operation to be performed (e.g., read, write, etc.).
flags: Additional flags for the command.
nsid: Identifies the namespace the command applies to.
metadata and addr: Pointers to metadata and data buffers, respectively.
metadata_len and data_len: Lengths of the metadata and data buffers.
cdw10 to cdw15: Command-specific parameters.
timeout_ms: Timeout for the command in milliseconds.
result: Stores the result of the command execution. 
*/

// bmp_BIN_test.c — Route B：黑白二值 + 主/裝置雙向驗證（少量輸出）
// build: gcc -O2 -Wall bmp_BIN_test.c -o EXE/bmp_BIN.exe
// run  : sudo ./EXE/bmp_BIN.exe

#define NVME_DEVICE "/dev/nvme0n1"
#define BMP_INPUT   "./DataSet/city_public.bmp"
#define BMP_OUTPUT  "./DataSet/output_bw.bmp"
#define OPCODE_CSD_BMP_BIN 0xC2

#define LBA_SIZE     512
#define PAGE_ALIGN   4096
#define MAX_CHUNK    (256 * 1024)    // 中段大小（會對齊到 unit）
#define RETRY_ONCE   1               // ioctl 失敗時退半一次
#define Logflag      1               // 1=列印；0=只印總結

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BITMAPFILEHEADER;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;    // 24
    uint32_t biCompression; // 0 (BI_RGB)
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BITMAPINFOHEADER;
#pragma pack(pop)

/* 小工具 */
static size_t gcd_sz(size_t a, size_t b){ while(b){ size_t t=a%b; a=b; b=t; } return a; }
static size_t lcm_sz(size_t a, size_t b){ return (a / gcd_sz(a,b)) * b; }
static size_t lcm3(size_t a, size_t b, size_t c){ return lcm_sz(lcm_sz(a,b), c); }
static size_t align_down(size_t x, size_t a){ return x - (x % a); }
static size_t align_up(size_t x, size_t a){ size_t r = x % a; return r ? (x + (a - r)) : x; }

static uint32_t sum_bytes(const uint8_t* p, size_t n){
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s;
}

/* 計算 end - start，回傳微秒，處理跨秒借位 */
static inline uint64_t ts_diff_us(const struct timespec* start, const struct timespec* end){
    time_t sec = end->tv_sec - start->tv_sec;
    long   ns  = end->tv_nsec - start->tv_nsec;
    if (ns < 0){ ns += 1000000000L; sec -= 1; }
    return (uint64_t)sec * 1000000ULL + (uint64_t)ns / 1000ULL;
}

/* 寄送一段，回傳 FEMU 段內處理後總和（DW0） */
static int send_chunk(int fd, uint8_t *ptr, size_t len,
                      uint32_t width, uint32_t abs_h, uint32_t stride,
                      uint8_t thr, uint32_t *dev_sum_out)
{
    if (len % LBA_SIZE) return -1;

    struct nvme_passthru_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode   = OPCODE_CSD_BMP_BIN;
    cmd.addr     = (uintptr_t)ptr;              // in-place
    cmd.data_len = (uint32_t)len;
    cmd.nsid     = 1;
    cmd.cdw12    = ((uint32_t)thr << 16) | (((uint32_t)(len / LBA_SIZE) - 1u) & 0xFFFFu);
    cmd.cdw13    = width;
    cmd.cdw14    = abs_h;
    cmd.cdw15    = (24u << 24) | (stride & 0x00FFFFFFu);

    int rc = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
    if (rc < 0) return -errno;
    if (dev_sum_out) *dev_sum_out = cmd.result; // FEMU 端回傳（DW0）
    return 0;
}

int main(void)
{
    /* 1) 開 NVMe + 讀 BMP header */
    int fd = open(NVME_DEVICE, O_RDWR);
    if (fd < 0) { perror("open NVMe device"); return 1; }

    FILE *fp = fopen(BMP_INPUT, "rb");
    if (!fp) { perror("open BMP file"); close(fd); return 1; }

    BITMAPFILEHEADER fh; BITMAPINFOHEADER ih;
    if (fread(&fh, sizeof(fh), 1, fp) != 1 || fread(&ih, sizeof(ih), 1, fp) != 1) {
        fprintf(stderr, "read header failed\n"); fclose(fp); close(fd); return 1;
    }
    if (fh.bfType != 0x4D42 || ih.biBitCount != 24 || ih.biCompression != 0) {
        fprintf(stderr, "Unsupported BMP (need 24-bit BI_RGB)\n"); fclose(fp); close(fd); return 1;
    }

    const uint32_t pixel_off = fh.bfOffBits;
    const int32_t  width     = ih.biWidth;
    const int32_t  height    = ih.biHeight;
    const uint32_t abs_h     = (height >= 0) ? (uint32_t)height : (uint32_t)(-height);
    const uint8_t  thr       = 128;

    const size_t row_bytes = (size_t)width * 3;
    const size_t stride    = (row_bytes + 3u) & ~3u;
    const size_t pixel_sz  = stride * abs_h;

    /* 2) 讀像素到頁對齊、且 512B 對齊的緩衝（尾端補 0） */
    const size_t total_len = align_up(pixel_sz, LBA_SIZE);
    uint8_t *pixel = (uint8_t*)aligned_alloc(PAGE_ALIGN, align_up(total_len, PAGE_ALIGN));
    if (!pixel) { perror("alloc"); fclose(fp); close(fd); return 1; }
    memset(pixel, 0, total_len);

    if (fseek(fp, pixel_off, SEEK_SET) != 0) { perror("fseek pixel"); free(pixel); fclose(fp); close(fd); return 1; }
    const size_t rd = fread(pixel, 1, pixel_sz, fp);
    fclose(fp);
    if (rd != pixel_sz) {
        fprintf(stderr, "pixel read short: %zu/%zu\n", rd, pixel_sz);
        free(pixel); close(fd); return 1;
    }

    printf("[Host] w=%d h=%d(abs=%u) row_bytes=%zu stride=%zu pixel_size=%zu total_len=%zu\n",
           width, height, abs_h, row_bytes, stride, pixel_sz, total_len);

    /* 3) 設定中段對齊單位：LCM(stride, 512, 4096) */
    const size_t unit  = lcm3(stride, LBA_SIZE, PAGE_ALIGN);
    size_t chunk       = align_down(MAX_CHUNK, unit);
    if (chunk == 0) chunk = unit;
    printf("[Host] unit=%zu chunk=%zu\n", unit, chunk);

    /* 4) 逐段送處理 + 驗證 + 計時 */
    size_t off = 0; unsigned seg = 0;

    unsigned   op_count    = 0;
    uint64_t   total_us    = 0ULL;
    uint64_t   total_bytes = 0ULL;

    #if Logflag
    /* 欄位標題，對齊輸出 */
        printf("[Host] %4s %10s %8s %14s %14s %-9s | %8s\n",
            "seg", "off", "len", "post_sum", "dev_sum", "check", "latency");
        printf("[Host] ---- ---------- -------- -------------- -------------- --------- | --------\n");
    #endif

    while (off < total_len) {
        size_t remain     = total_len - off;
        const int middle  = (remain > chunk);
        size_t want       = middle ? chunk : remain;

        size_t this_chunk = middle ? align_down(want, unit) : align_down(want, LBA_SIZE);
        if (this_chunk == 0) this_chunk = (remain >= LBA_SIZE) ? LBA_SIZE : remain;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        uint32_t dev_sum = 0;
        int rc = send_chunk(fd, pixel + off, this_chunk,
                            (uint32_t)width, abs_h, (uint32_t)stride,
                            thr, &dev_sum);

        if (rc && RETRY_ONCE) {
            size_t half = align_down(this_chunk / 2, middle ? unit : LBA_SIZE);
            if (half >= LBA_SIZE) {
                rc = send_chunk(fd, pixel + off, half,
                                (uint32_t)width, abs_h, (uint32_t)stride,
                                thr, &dev_sum);
                if (!rc) this_chunk = half;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint64_t us = ts_diff_us(&t0, &t1);

        if (rc) {
            fprintf(stderr, "[Host] seg=%u ioctl fail at off=%zu len=%zu: %s\n",
                    seg, off, this_chunk, strerror(-rc));
            free(pixel); close(fd); return 1;
        }

        const uint32_t post_host_sum = sum_bytes(pixel + off, this_chunk);

        #if Logflag
            printf("[Host] %4u %10zu %8zu %14u %14u %-9s | %4" PRIu64 " us\n",
                seg, off, this_chunk, post_host_sum, dev_sum,
                (post_host_sum == dev_sum) ? "OK" : "MISMATCH", us);
        #endif

        if (post_host_sum != dev_sum) { free(pixel); close(fd); return 2; }

        off         += this_chunk;
        seg         += 1;
        op_count    += 1;
        total_us    += us;
        total_bytes += this_chunk;
    }

    /* 統計輸出 */
    double total_sec = total_us / 1e6;
    double mib       = total_bytes / (1024.0 * 1024.0);
    double avg_us    = op_count ? ((double)total_us / op_count) : 0.0;
    double avg_bw    = (total_sec > 0.0) ? (mib / total_sec) : 0.0;

    printf("[Host_Filter] Ops=%u | Bytes=%.2f MiB | Total: %" PRIu64
           " us | Avg/op: %.0f us  | Avg BW: %.2f MiB/s\n",
           op_count, mib, (uint64_t)total_us, avg_us, avg_bw);

    /* 5) 寫回 BMP（header 原封不動，只寫原始 pixel_sz） */
    FILE *out = fopen(BMP_OUTPUT, "wb");
    if (!out) { perror("open output"); free(pixel); close(fd); return 1; }
    fwrite(&fh, 1, sizeof(fh), out);
    fwrite(&ih, 1, sizeof(ih), out);
    long cur = ftell(out);
    if ((uint32_t)cur < fh.bfOffBits) {
        uint32_t pad = fh.bfOffBits - (uint32_t)cur;
        static uint8_t z[64];
        while (pad) {
            uint32_t w = (pad > sizeof(z)) ? (uint32_t)sizeof(z) : pad;
            fwrite(z, 1, w, out); pad -= w;
        }
    }
    fwrite(pixel, 1, pixel_sz, out);
    fclose(out);

    printf("[Host] Done -> %s\n", BMP_OUTPUT);
    free(pixel);
    close(fd);
    return 0;
}
