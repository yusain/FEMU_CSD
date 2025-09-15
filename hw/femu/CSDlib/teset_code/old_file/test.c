#include <fcntl.h>
#include <stdio.h>
#include <linux/nvme_ioctl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

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


int main() {
    // 開啟 NVMe 裝置（命名空間）
    int fd = open("/dev/nvme0n1", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // 分配一塊 4KB 且 4096-byte 對齊的記憶體緩衝區，用來接收資料
    void *buffer = aligned_alloc(4096, 4096); // 4KB 對齊
    if (!buffer) {
        perror("aligned_alloc");
        close(fd);
        return 1;
    }
    memset(buffer, 0, 4096); // 將緩衝區清零

    FILE *f = fopen("/home/yusian/temp/test.txt", "rb");
    if (!f) {
        perror("open test.txt");
        free(buffer);
        close(fd);
        return 1;
    }
    fread(buffer, 1, 4096, f);
    fclose(f);

    // 準備自訂的 NVMe I/O 命令結構
    struct nvme_passthru_cmd cmd = {
        .opcode = 0xC0,               // 自訂的 NVMe 指令代碼（可在 QEMU 中攔截處理）
        .nsid = 1,                    // 命名空間 ID（一般為 1）
        .addr = (intptr_t)buffer,     // 用於資料傳輸的實體記憶體位址
        .data_len = 4096,             // 傳輸的資料長度（此例為 4KB）
        .cdw10 = 0,                   // Command Dword 10，可做為起始 LBA 或自訂參數
        .cdw12 = 0,                   // Command Dword 12，這裡可表示傳輸區塊數量 - 1
    };

    // 執行 ioctl 系統呼叫，傳送 I/O 指令到 NVMe 驅動程式
    if (ioctl(fd, NVME_IOCTL_IO_CMD, &cmd) == -1) {
        perror("ioctl"); // 若發生錯誤，顯示錯誤訊息
    } else {
        printf("自訂 NVMe I/O 指令執行成功！\\n");
        // 顯示回傳資料（前 16 個位元組）
        for (int i = 0; i < 16; i++) {
            printf("%02x ", ((unsigned char *)buffer)[i]);
        }
        printf("\\n");
    }

    // 釋放緩衝區與關閉裝置
    free(buffer);
    close(fd);
    return 0;
}
