#include <fcntl.h>
#include <stdio.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#define BLOCK_SIZE 4096
#define BLOCK_COUNT 1

// 印出 buffer 內容（十六進位與 ASCII）
void print_buffer(const unsigned char *buf, size_t len) {
    printf("Buffer virtual address: %p\n", buf);
    
    printf("Buffer (hex):\n");
    for (size_t i = 0; i < len; ++i) {
        printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\nBuffer (ASCII):\n");
    for (size_t i = 0; i < len; ++i) {
        printf("%c",  buf[i] );
        if ((i + 1) % 64 == 0) printf("\n");
    }
    printf("\n");
}

int main() {
    int fd = open("/dev/nvme0n1", O_RDWR);
    if (fd < 0) {
        perror("open nvme device");
        return 1;
    }

    // 分配一塊 4KB 且 4096-byte 對齊的記憶體緩衝區，用來接收資料
    void *buffer = aligned_alloc(4096, 4096); // 4KB 對齊
    if (!buffer) {
        perror("aligned_alloc");
        close(fd);
        return 1;
    }
    memset(buffer, 0, BLOCK_SIZE); // 將緩衝區清零

    FILE *randf = fopen("/dev/random", "rb");
    if (!randf) {
        perror("open /dev/random");
        free(buffer);
        close(fd);
        return 1;
    }

    for (int i = 0; i < BLOCK_COUNT; ++i) {
        memset(buffer, 0, BLOCK_SIZE); // 將緩衝區清零
        if (fread(buffer, 1, BLOCK_SIZE, randf) != BLOCK_SIZE) {
            perror("fread /dev/random");
            break;
        }

        struct nvme_passthru_cmd cmd = {
            .opcode = 0xC0,               // 自訂的 NVMe 指令代碼（可在 QEMU 中攔截處理）
            .nsid = 1,                    // 命名空間 ID（一般為 1）
            .addr = (intptr_t)buffer,     // 用於資料傳輸的實體記憶體位址
            .data_len = BLOCK_SIZE,       // 傳輸的資料長度（此例為 4KB）
            .cdw10 = i,                   // Command Dword 10，可做為起始 LBA 或自訂參數
            .cdw12 = BLOCK_SIZE/512 -1,      // Command Dword 12，這裡可表示傳輸區塊數量 - 1
        };

        // 印出 buffer 內容
        printf("原始數值，LBA %d 的資料:\n", i);
        print_buffer((unsigned char*)buffer, BLOCK_SIZE);
        int ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
        if (ret < 0) {
            perror("ioctl NVME_IOCTL_IO_CMD");
            break;
        }
        printf("filter後，LBA %d 的資料:\n", i);
        print_buffer((unsigned char*)buffer, BLOCK_SIZE);
    }

    fclose(randf);
    free(buffer);
    close(fd);
    return 0;
}