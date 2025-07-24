#include "../nvme.h"
#include "./ftl.h"

static void bb_init_ctrl_str(FemuCtrl *n)
{
    static int fsid_vbb = 0;
    const char *vbbssd_mn = "FEMU BlackBox-SSD Controller";
    const char *vbbssd_sn = "vSSD";

    nvme_set_ctrl_name(n, vbbssd_mn, vbbssd_sn, &fsid_vbb);
}

/* bb <=> black-box */
static void bb_init(FemuCtrl *n, Error **errp)
{
    struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));

    bb_init_ctrl_str(n);

    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    femu_debug("Starting FEMU in Blackbox-SSD mode ...\n");
    ssd_init(n);
}

static void bb_flip(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);

    switch (cdw10) {
    case FEMU_ENABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = true;
        femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = false;
        femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_ENABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = NAND_READ_LATENCY;
        ssd->sp.pg_wr_lat = NAND_PROG_LATENCY;
        ssd->sp.blk_er_lat = NAND_ERASE_LATENCY;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = 0;
        ssd->sp.pg_wr_lat = 0;
        ssd->sp.blk_er_lat = 0;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_RESET_ACCT:
        n->nr_tt_ios = 0;
        n->nr_tt_late_ios = 0;
        femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", n->devname,
                n->nr_tt_late_ios, n->nr_tt_ios);
        break;
    case FEMU_ENABLE_LOG:
        n->print_log = true;
        femu_log("%s,Log print [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_LOG:
        n->print_log = false;
        femu_log("%s,Log print [Disabled]!\n", n->devname);
        break;
    default:
        printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
    }
}

static uint16_t bb_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
}

#include <time.h>
static uint64_t csd_total_time_us = 0;
static uint64_t csd_total_calls   = 0;
static uint64_t csd_total_bytes   = 0;
static uint64_t csd_filtered_bytes = 0; // 非 ASCII printable bytes


static uint16_t CSD_Filter(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start); // 開始計時

    // nvme-io.c->nvme_rw
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint16_t ctrl = le16_to_cpu(rw->control);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint64_t prp1 = le64_to_cpu(rw->prp1);
    uint64_t prp2 = le64_to_cpu(rw->prp2);

    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint16_t ms = le16_to_cpu(ns->id_ns.lbaf[lba_index].ms);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].lbads;
    uint64_t data_size = (uint64_t)nlb << data_shift;
    uint64_t data_offset = slba << data_shift;
    uint64_t meta_size = nlb * ms;
    uint64_t elba = slba + nlb;
    uint16_t err;

    if (femu_nvme_rw_check_req(n, ns, cmd, req, slba, elba, nlb, ctrl, data_size, meta_size))
        return err;

    if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, n)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, prp1), 0, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    assert((nlb << data_shift) == req->qsg.size);

    req->slba = slba;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;

    CSD_debug("[YS_CSD_Filter]: cmd->opcode = %x, slba = %lu, nlb = %u, prp1 = 0x%016lx, prp2 = 0x%016lx\n",
        rw->opcode, slba, nlb, le64_to_cpu(rw->prp1), le64_to_cpu(rw->prp2)); 

    // dram.c->backend_rw
    int sg_cur_index = 0;
    dma_addr_t sg_cur_byte = 0;
    uint64_t mb_oft = data_offset;
    dma_addr_t cur_addr, cur_len;
    void *mb = n->mbe->logical_space;
    QEMUSGList *qsg = &req->qsg;
    
    uint64_t filtered_cnt = 0;

    CSD_debug("qsg->nsg = %d, qsg->size = %lu, data_size = %lu\n", qsg->nsg, qsg->size, data_size);
    while (sg_cur_index < qsg->nsg) {
        cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
        cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;

        unsigned char *data = (unsigned char *)(mb + mb_oft);
        
        CSD_debug("sg_cur_index=%d, cur_addr=0x%lx, cur_len=%lu, mb_oft=%lu\n",
                                     sg_cur_index, cur_addr, cur_len, mb_oft);

        // 將主機 buffer 資料 DMA 讀入設備 buffer
        dma_memory_read(qsg->as, cur_addr, data, cur_len, MEMTXATTRS_UNSPECIFIED);
        
        // 準備過濾後 buffer
        unsigned char *filtered_buf = malloc(cur_len);
        if (!filtered_buf) {
            CSD_debug("filtered_buf malloc failed\n");
            return 1;
        }

        // 印出原始資料 (hex)
        CSD_debug("所有資料區塊:\n");
        for (uint64_t i = 0; i < cur_len; i++) {
            CSD_debug("%02X%c", data[i], (i + 1) % 32 ? ' ' : '\n');
        }
        
        CSD_debug("過濾區塊:\n");
        for (uint64_t i = 0; i < cur_len; i++) {
            if (data[i] >= 32 && data[i] <= 126) {
                filtered_buf[i] = data[i];
                filtered_cnt++;
            } else {
                filtered_buf[i] = '*';
            }
            CSD_debug("%c%c", filtered_buf[i], (i + 1) % 32 ? ' ' : '\n');
        }
        CSD_debug("\n");

        // 將過濾後資料寫回主機 buffer
        dma_memory_write(qsg->as, cur_addr, filtered_buf, cur_len, MEMTXATTRS_UNSPECIFIED);

        free(filtered_buf);

        sg_cur_byte += cur_len;
        if (sg_cur_byte == qsg->sg[sg_cur_index].len) {
            sg_cur_byte = 0;
            ++sg_cur_index;
        }
        mb_oft += cur_len;

        // 結束計時
        clock_gettime(CLOCK_MONOTONIC, &end); 
        long diff_us = (end.tv_sec - start.tv_sec) * 1000000L +
                    (end.tv_nsec - start.tv_nsec) / 1000L;

        // 更新統計變數
        csd_total_time_us    += diff_us;
        csd_total_calls      += 1;
        csd_total_bytes      += data_size;
        csd_filtered_bytes   += filtered_cnt;

        // 輸出單次執行資訊
        CSD_debug("[YS_CSD_Filter]: Filter 執行時間 = %ld us\n\n", diff_us);
        CSD_debug("[YS_CSD_Filter] time: %ld us | size: %lu B | filtered: %lu B (%.2f%%)\n",
                diff_us, data_size, filtered_cnt,
                data_size ? (100.0 * filtered_cnt / data_size) : 0.0);
    }
    //qemu_sglist_destroy(qsg);
    return 0;
}


#include "tiny_sha256.h"

static uint16_t CSD_Checksum(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                             NvmeRequest *req)
{
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint16_t ctrl = le16_to_cpu(rw->control);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint64_t prp1 = le64_to_cpu(rw->prp1);
    uint64_t prp2 = le64_to_cpu(rw->prp2);

    const uint8_t lba_index = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint16_t ms = le16_to_cpu(ns->id_ns.lbaf[lba_index].ms);
    const uint8_t data_shift = ns->id_ns.lbaf[lba_index].lbads;
    uint64_t data_size = (uint64_t)nlb << data_shift;
    uint64_t data_offset = slba << data_shift;
    uint64_t meta_size = nlb * ms;
    uint64_t elba = slba + nlb;
    uint16_t err;

    if (femu_nvme_rw_check_req(n, ns, cmd, req, slba, elba, nlb, ctrl, data_size, meta_size))
        return err;

    if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, n)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, prp1), 0, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    assert((nlb << data_shift) == req->qsg.size);

    req->slba = slba;
    req->status = NVME_SUCCESS;
    req->nlb = nlb;

    CSD_debug("[YS_CSD_Checksum]: cmd->opcode = %x, slba = %lu, nlb = %u, prp1 = 0x%016lx, prp2 = 0x%016lx\n",
        rw->opcode, slba, nlb, prp1, prp2);

    // dma buffer 讀取與 hash 計算
    int sg_cur_index = 0;
    dma_addr_t sg_cur_byte = 0;
    uint64_t mb_oft = data_offset;
    dma_addr_t cur_addr, cur_len;
    void *mb = n->mbe->logical_space;
    QEMUSGList *qsg = &req->qsg;

    tiny_sha256_ctx sha256;
    tiny_sha256_init(&sha256);

    CSD_debug("qsg->nsg = %d, qsg->size = %lu, data_size = %lu\n", qsg->nsg, qsg->size, data_size);

    while (sg_cur_index < qsg->nsg) {
        cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
        cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;

        unsigned char *data = (unsigned char *)(mb + mb_oft);

        CSD_debug("sg_cur_index=%d, cur_addr=0x%lx, cur_len=%lu, mb_oft=%lu\n",
                  sg_cur_index, cur_addr, cur_len, mb_oft);

        // DMA 讀入
        dma_memory_read(qsg->as, cur_addr, data, cur_len, MEMTXATTRS_UNSPECIFIED);

        // 印出原始資料 (hex)
        CSD_debug("印出前 64 bytes (Hex):\n");
        for (size_t i = 0; i < 64; ++i) {
            CSD_debug("%02X%c", data[i], (i + 1) % 16 ? ' ' : '\n');
        }

        // 累積進 SHA-256
        tiny_sha256_update(&sha256, data, cur_len);

        sg_cur_byte += cur_len;
        if (sg_cur_byte == qsg->sg[sg_cur_index].len) {
            sg_cur_byte = 0;
            ++sg_cur_index;
        }
        mb_oft += cur_len;
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    tiny_sha256_final(&sha256, hash);

    // 印出 SHA-256 hash 結果
    CSD_debug("\n[YS_CSD_Checksum]: SHA-256 = ");
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        CSD_debug("%02x", hash[i]);
    }
    CSD_debug("\n");

    clock_gettime(CLOCK_MONOTONIC, &end);
    long diff_us = (end.tv_sec - start.tv_sec) * 1000000L +
                   (end.tv_nsec - start.tv_nsec) / 1000L;

    csd_total_time_us += diff_us;
    csd_total_calls += 1;
    csd_total_bytes += data_size;

    CSD_debug("[YS_CSD_Checksum] time: %ld us | size: %lu B\n", diff_us, data_size);

    return NVME_SUCCESS;
}


static uint16_t bb_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                          NvmeRequest *req)
{
    //femu_log("bb_io_cmd: cmd->opcode = %x\r\n", cmd->opcode);
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        //CSD_Filter(n, ns, cmd, req); //可用dd但數據不易觀察
        return bb_nvme_rw(n, ns, cmd, req);

    case CSD_CMD_Filter:
        CSD_debug("\rCSD_CMD_Filter\n");
        CSD_Filter(n, ns, cmd, req);
        return NVME_SUCCESS;
    case CSD_CMD_Checksum:
        CSD_debug("\rCSD_CMD_Checksum\n");
        CSD_Checksum(n, ns, cmd, req);
        return NVME_SUCCESS;
    case CSD_CMD_RESET:
        // 重設統計
        CSD_debug("\rCSD_CMD_RESET\n");
        csd_total_time_us = 0;
        csd_total_calls = 0;
        csd_total_bytes = 0;
        csd_filtered_bytes = 0;
        return NVME_SUCCESS;
    case CSD_CMD_REPORT:
        // 輸出統計資訊
        CSD_debug("\rCSD_CMD_REPORT\n");
        double avg_us = (double)csd_total_time_us / csd_total_calls;
        double bw_mb_s = (double)csd_total_bytes / csd_total_time_us; // MB/s ≈ bytes/us
        double filter_pct = csd_total_bytes ? (100.0 * csd_filtered_bytes / csd_total_bytes) : 0.0;

        femu_log("=== CSD Filter Summary ===\n");
        femu_log("  Total Ops      : %lu\n", csd_total_calls);
        femu_log("  Total Bytes    : %lu\n", csd_total_bytes);
        femu_log("  Total Time     : %lu us\n", csd_total_time_us);
        femu_log("  Avg Time/op    : %.2f us\n", avg_us);
        femu_log("  Avg BW         : %.2f MB/s\n", bw_mb_s);
        femu_log("  Filtered Bytes : %lu (%.2f%%)\n", csd_filtered_bytes, filter_pct);
        return NVME_SUCCESS;

    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t bb_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case NVME_ADM_CMD_FEMU_FLIP:
        bb_flip(n, cmd);
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

int nvme_register_bbssd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = bb_init,
        .exit             = NULL,
        .rw_check_req     = NULL,
        .admin_cmd        = bb_admin_cmd,
        .io_cmd           = bb_io_cmd,
        .get_log          = NULL,
    };

    return 0;
}

