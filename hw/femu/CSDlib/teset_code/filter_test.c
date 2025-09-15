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

//*下述傳輸量共 512K Bytes
#define BLOCK_SIZE 1024 * 512       // 測試資料區塊大小
#define BLOCK_COUNT 1                // 測試次數*/
/*
#define BLOCK_SIZE 1024 * 4          // 測試資料區塊大小
#define BLOCK_COUNT 128              // 測試次數*/
/*
#define BLOCK_SIZE 512               // 測試資料區塊大小
#define BLOCK_COUNT 1024             // 測試次數*/

// 印出 buffer 內容（十六進位與 ASCII）
void print_buffer(const unsigned char *buf, size_t len) {
    printf("Buffer virtual address: %p\n", buf);
    
    printf("Buffer (hex):\n");
    for (size_t i = 0; i < len; ++i) {
        printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\nBuffer (ASCII):\n");
    for (size_t i = 0; i < 32; ++i) {
        printf("%c",  buf[i] );
        if ((i + 1) % 64 == 0) printf("\n");
    }
    printf("\n");
}

// ===== 主機過濾邏輯 =====
void buf_filter(unsigned char *buffer, size_t size) {
    struct timespec start, comput_end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    // 分配一塊記憶體緩衝區，用來過濾資料
    unsigned char *filtered_buf = malloc(size);
    if (!filtered_buf) {
        perror("aligned_alloc");
	return;
    }

    unsigned long filtered_cnt = 0;

    for (size_t i = 0; i < size; ++i) {
        if (buffer[i] >= 32 && buffer[i] <= 126) {
	    filtered_buf[i] = buffer[i];
            filtered_cnt++;
        } else {
            filtered_buf[i] = '*';
        }
    }
    free(filtered_buf);
            
    clock_gettime(CLOCK_MONOTONIC, &comput_end);
    long latency_us = (comput_end.tv_sec - start.tv_sec) * 1000000 + (comput_end.tv_nsec - start.tv_nsec) / 1000;
    printf("[Host_Filter] size: %zu B | filtered: %lu B (%.2f%%) | Filter latency: %ld us\n",
           size, filtered_cnt, size ? (100.0 * filtered_cnt / size) : 0.0, latency_us);
}

int main() {
    int fd = open("/dev/nvme0n1", O_RDWR);
    if (fd < 0) {
        perror("open nvme device");
        return 1;
    }

    // 分配一塊 4KB 且 4096-byte 對齊的記憶體緩衝區，用來接收資料
    void *buffer = aligned_alloc(4096, BLOCK_SIZE); // 4KB 對齊
    //void *out_buf = aligned_alloc(4096, size);
    if (!buffer) {
        perror("aligned_alloc");
        close(fd);
        return 1;
    }
    memset(buffer, 0, BLOCK_SIZE); // 將緩衝區清零

    //FILE *randf = fopen("/dev/zero", "rb");   // TEST_dataA
    //FILE *randf = fopen("/home/yusian/CSD/teset_code/TEST_Data/TEST_dataB", "rb");
    FILE *randf = fopen("/dev/random", "rb"); // TEST_dataC

    if (!randf) {
        perror("open randf file err");
        free(buffer);
        close(fd);
        return 1;
    }
    
    struct timespec start, end;
    long Total_cmd_latency_us = 0;

    for (int i = 0; i < BLOCK_COUNT; ++i) {
        memset(buffer, 0, BLOCK_SIZE); // 將緩衝區清零
        if (fread(buffer, 1, BLOCK_SIZE, randf) != BLOCK_SIZE) {
            perror("fread randf file err");
            break;
        }
	
        //printf("=== Sending to device (LBA %d) ===\n", i);
        //buf_filter((unsigned char*)buffer, BLOCK_SIZE);
        // printf("原始數值，LBA %d 的資料:\n", i);
        //print_buffer((unsigned char*)buffer, BLOCK_SIZE);



	    clock_gettime(CLOCK_MONOTONIC, &start);
        struct nvme_passthru_cmd cmd = {
            .opcode = 0xC0,               // 自訂的 NVMe 指令代碼（可在 QEMU 中攔截處理）
            .nsid = 1,                    // 命名空間 ID（一般為 1）
            .addr = (intptr_t)buffer,     // 用於資料傳輸的實體記憶體位址
            .data_len = BLOCK_SIZE,       // 傳輸的資料長度（此側資可為4K、12K、40K 等）
            .cdw10 = i,                   // Command Dword 10，可做為起始 LBA 或自訂參數
            .cdw12 = BLOCK_SIZE/512 -1,   // Command Dword 12，將傳輸區塊數量分塊 - 1
        };
        
        // 執行 ioctl 系統呼叫，傳送 I/O 指令到 NVMe 驅動程式
        if (ioctl(fd, NVME_IOCTL_IO_CMD, &cmd) < 0) {
            perror("ioctl"); // 若發生錯誤，顯示錯誤訊息
            return 1;
        } else {
	    // printf("自訂 NVMe I/O 指令執行成功！\n\n");
        }
	    clock_gettime(CLOCK_MONOTONIC, &end);
        Total_cmd_latency_us += (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;

        // 印出 buffer 內容
        printf("filter後，LBA %d 的資料:\n", i);
        print_buffer((unsigned char*)buffer, BLOCK_SIZE);

    }

    // 平均耗時
    printf("[Host_Filter] HOST CMD Times => Total Time: %ld us | Avg Time/op: %ld us, Avg BW: %.2lf MB/s \n",
        Total_cmd_latency_us, BLOCK_COUNT ? Total_cmd_latency_us / BLOCK_COUNT : 0,
	   (double)BLOCK_SIZE / Total_cmd_latency_us);

    struct nvme_passthru_cmd report_cmd = {0};
    report_cmd.opcode = 0xCE; // CSD_CMD_REPORT
    report_cmd.nsid = 1;
    if (ioctl(fd, NVME_IOCTL_IO_CMD, &report_cmd) < 0) {
        fprintf(stderr, "ioctl REPORT error (%d): %s\n", errno, strerror(errno));
    }

    struct nvme_passthru_cmd reset_cmd = {0};
    reset_cmd.opcode = 0xCF; // CSD_CMD_RESET
    reset_cmd.nsid = 1;
    if (ioctl(fd, NVME_IOCTL_IO_CMD, &reset_cmd) < 0) {
        fprintf(stderr, "ioctl RESET error (%d): %s\n", errno, strerror(errno));
    } else {
        printf("CSD REPORT 完成，可印出回傳資料進行分析\n");
    }

    fclose(randf);
    free(buffer);
    close(fd);
    return 0;
}
