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

// ---- 最終版 CSD_Filter ----
// mode=1: Bit-count  -> 回傳 '1' 的數量（不改 buffer）
// mode=2: ASCII      -> 只保留可見 ASCII，縮短；寫回 PRP 起點，DW0=輸出長度
// mode=3: DNA(FASTA) -> 只保留 A/C/G/T，移除 header 與所有非 A/C/G/T（含空白、數字、N…）；
//                       不插入換行，寫回 PRP 起點，DW0=輸出長度

// helper：把 src[0..len) 寫回 PRP (qsg) 起點，連續覆蓋
static void qsg_write_from_buf(QEMUSGList *qsg, const uint8_t *src, uint64_t len)
{
    int sg_idx = 0; dma_addr_t sg_off = 0; uint64_t off = 0;
    while (sg_idx < qsg->nsg && off < len) {
        dma_addr_t cur = qsg->sg[sg_idx].base + sg_off;
        uint64_t avail = qsg->sg[sg_idx].len - sg_off;
        uint64_t n = MIN(avail, len - off);
        dma_memory_write(qsg->as, cur, (void*)(src + off), n, MEMTXATTRS_UNSPECIFIED);
        off += n; sg_off += n;
        if (sg_off == qsg->sg[sg_idx].len) { sg_off = 0; ++sg_idx; }
    }
}

// ---- CSD_Filter ----
static uint16_t CSD_Filter(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);

    NvmeRwCmd *rw = (NvmeRwCmd*)cmd;
    uint16_t ctrl = le16_to_cpu(rw->control);
    uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint64_t prp1 = le64_to_cpu(rw->prp1);
    uint64_t prp2 = le64_to_cpu(rw->prp2);

    const uint8_t  lba_idx   = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t  lbads     = ns->id_ns.lbaf[lba_idx].lbads;
    const uint16_t ms        = le16_to_cpu(ns->id_ns.lbaf[lba_idx].ms);
    const uint64_t data_size = (uint64_t)nlb << lbads;
    const uint64_t meta_size = (uint64_t)nlb * ms;
    const uint64_t elba      = slba + nlb;

    const uint8_t  mode      = (uint8_t)(cmd->cdw15 & 0xFF);
    uint64_t       valid_len = (uint64_t)le32_to_cpu(cmd->cdw10);
    if (!valid_len || valid_len > data_size) valid_len = data_size;

    uint16_t err;
    if (femu_nvme_rw_check_req(n, ns, cmd, req, slba, elba, nlb, ctrl, data_size, meta_size))
        return err;

    if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, n)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, prp1), 0, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    req->slba = slba; req->status = NVME_SUCCESS; req->nlb = nlb;

    int sg_idx = 0; dma_addr_t sg_off = 0;
    uint8_t *mb = (uint8_t*)n->mbe->logical_space;
    uint64_t mb_ofs = slba << lbads;
    QEMUSGList *qsg = &req->qsg;

    uint64_t done = 0;
    uint64_t result = 0;

    // DNA/ASCII 「縮短」需要一次累積再回寫
    uint8_t *out_all = NULL; uint64_t out_len = 0;
    if (mode == 2 || mode == 3) {
        out_all = g_malloc(valid_len);             // 上限即原長
        if (!out_all) return NVME_INTERNAL_DEV_ERROR;
    }

    // DNA：行首/標頭狀態（host 仍以「行對齊」打包，本狀態只在單次命令內使用）
    bool at_line_start = true, in_header = false;

    while (sg_idx < qsg->nsg && done < valid_len) {
        dma_addr_t cur = qsg->sg[sg_idx].base + sg_off;
        uint64_t avail = qsg->sg[sg_idx].len - sg_off;
        uint64_t take  = MIN(avail, valid_len - done);

        uint8_t *buf = mb + mb_ofs;
        dma_memory_read(qsg->as, cur, buf, take, MEMTXATTRS_UNSPECIFIED);

        if (mode == 1) {
            // Bit-count：只計數
            for (uint64_t i=0;i<take;i++){
                uint8_t c = buf[i];
                result += (c=='0' || c=='1' || c==' ' || c=='\n' || c=='\r')
                          ? (c=='1')
                          : __builtin_popcount(c);      // 相容位元流
            }

        } else if (mode == 2) {
            // ASCII-shrink：只保留可見 ASCII(32..126)
            for (uint64_t i=0;i<take;i++){
                uint8_t c = buf[i];
                if (c >= 32 && c <= 126) out_all[out_len++] = c;
            }

        } else if (mode == 3) {
            // DNA-strip：移除 header；只保留 A/C/G/T；不輸出換行
            for (uint64_t i=0;i<take;i++){
                uint8_t c = buf[i];
                if (c == '\r') continue;
                if (c == '\n') { at_line_start = true; in_header = false; continue; }
                if (at_line_start) {
                    at_line_start = false;
                    if (c == '>') { in_header = true; continue; }  // 整行略過
                }
                if (in_header) continue;

                // 保留 A/C/G/T（大小寫皆可；不改大小寫）
                if (c=='A'||c=='C'||c=='G'||c=='T'||
                    c=='a'||c=='c'||c=='g'||c=='t') {
                    out_all[out_len++] = c;
                }
            }
        }

        done   += take;
        sg_off += take;
        if (sg_off == qsg->sg[sg_idx].len) { sg_off = 0; ++sg_idx; }
        mb_ofs += take;
    }

    if (mode == 2 || mode == 3) {
        qsg_write_from_buf(qsg, out_all, out_len);   // 覆蓋回 PRP 起點
        result = out_len;
        g_free(out_all);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us = (t1.tv_sec - t0.tv_sec)*1000000L + (t1.tv_nsec - t0.tv_nsec)/1000L;

    csd_total_time_us  += us;
    csd_total_calls    += 1;
    csd_total_bytes    += valid_len;

    req->cqe.n.result = cpu_to_le32((uint32_t)result);
    CSD_debug("[CSD_Filter] mode=%u | in_valid=%lu | out=%lu | %ld us\n",
              mode, valid_len, result, us);
    return NVME_SUCCESS;
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

    /*CSD_debug("[YS_CSD_Filter]: cmd->opcode = %x, slba = %lu, nlb = %u, prp1 = 0x%016lx, prp2 = 0x%016lx\n",
    rw->opcode, slba, nlb, le64_to_cpu(rw->prp1), le64_to_cpu(rw->prp2)); */

    // 準備計算 SHA-256
    int sg_cur_index = 0;
    dma_addr_t sg_cur_byte = 0;
    uint64_t mb_oft = data_offset;
    dma_addr_t cur_addr, cur_len;
    void *mb = n->mbe->logical_space;
    QEMUSGList *qsg = &req->qsg;

    tiny_sha256_ctx sha256;
    tiny_sha256_init(&sha256);

    while (sg_cur_index < qsg->nsg) {
        cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
        cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;

        unsigned char *data = (unsigned char *)(mb + mb_oft);

        // DMA 讀入
        dma_memory_read(qsg->as, cur_addr, data, cur_len, MEMTXATTRS_UNSPECIFIED);

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

    // 將 SHA256 hash 前 4 bytes 存到 result 供 host 驗證
    uint32_t hash32;
    memcpy(&hash32, hash, sizeof(uint32_t));
    req->cqe.n.result = hash32;

    clock_gettime(CLOCK_MONOTONIC, &end);
    long diff_us = (end.tv_sec - start.tv_sec) * 1000000L +
                   (end.tv_nsec - start.tv_nsec) / 1000L;

    // 更新統計變數
    csd_total_time_us += diff_us;
    csd_total_calls += 1;
    csd_total_bytes += data_size;

    /*CSD_debug("[YS_CSD_Checksum] time: %ld us | size: %lu B | result=0x%08x\n",
              diff_us, data_size, hash32);*/

    return NVME_SUCCESS;
}


// device — 逐像素黑白二值（B=G=R=0/255），每段回傳處理後總和(DW0)
static uint16_t CSD_BMP_BIN(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd, NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    const uint16_t ctrl = le16_to_cpu(rw->control);
    const uint32_t nlb  = le16_to_cpu(rw->nlb) + 1;
    const uint64_t slba = le64_to_cpu(rw->slba);
    const uint64_t prp1 = le64_to_cpu(rw->prp1);
    const uint64_t prp2 = le64_to_cpu(rw->prp2);

    const uint8_t  lba_index  = NVME_ID_NS_FLBAS_INDEX(ns->id_ns.flbas);
    const uint8_t  data_shift = ns->id_ns.lbaf[lba_index].lbads;
    const uint64_t data_size  = (uint64_t)nlb << data_shift;
    const uint64_t data_offset= ((uint64_t)le64_to_cpu(rw->slba)) << data_shift;

    uint16_t err = femu_nvme_rw_check_req(n, ns, cmd, req, slba, slba + nlb, nlb, ctrl, data_size, 0);
    if (err) {
        return err;
    }
    if (nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, data_size, n)) {
        nvme_set_error_page(n, req->sq->sqid, cmd->cid, NVME_INVALID_FIELD,
                            offsetof(NvmeRwCmd, prp1), 0, ns->id);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    /* ---- 主機參數 ---- */
    const uint32_t width    = le32_to_cpu(cmd->cdw13);
    const uint32_t abs_h    = le32_to_cpu(cmd->cdw14);
    const uint32_t cdw15    = le32_to_cpu(cmd->cdw15);
    const uint32_t bpp      = (cdw15 >> 24) & 0xFF;           /* 24 */
    const uint32_t stride   = (cdw15 & 0x00FFFFFF);           /* 0 = auto(4-byte aligned) */
    const uint32_t cdw12    = le32_to_cpu(cmd->cdw12);
    const uint8_t  thr      = (cdw12 >> 16) & 0xFF;           /* 門檻(0..255) */

    const uint64_t row_bytes= (uint64_t)width * 3u;
    const uint64_t stride_ex= (stride == 0) ? ((row_bytes + 3) & ~3ULL) : stride;

    if (bpp != 24 || width == 0 || abs_h == 0 || stride_ex < row_bytes) {
        CSD_debug("[CSD_BMP_BIN] bad params w=%u h=%u bpp=%u row=%llu stride=%llu\n",
                  (unsigned)width, (unsigned)abs_h, (unsigned)bpp,
                  (unsigned long long)row_bytes, (unsigned long long)stride_ex);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    /* 命令摘要（一次）*/
    CSD_debug("[CSD_BMP_BIN][CMD] slba=%llu nlb=%u bytes=%llu | w=%u h=%u row=%llu stride=%llu thr=%u\n",
              (unsigned long long)slba, (unsigned)nlb, (unsigned long long)data_size,
              (unsigned)width, (unsigned)abs_h,
              (unsigned long long)row_bytes, (unsigned long long)stride_ex, (unsigned)thr);

    /* 逐 SG；維持列位置與像素內狀態（跨 SG 對齊） */
    int sg_idx = 0;
    dma_addr_t sg_byte = 0;
    uint64_t mb_oft = data_offset;
    uint8_t *mb = (uint8_t *)n->mbe->logical_space;
    QEMUSGList *qsg = &req->qsg;

    uint64_t row_pos = 0;            /* 0..stride_ex-1（row_bytes 內為像素資料，後面是 padding） */
    uint16_t sum3 = 0;               /* 累積 B+G+R（最多 765） */
    uint8_t  ch   = 0;               /* 目前像素通道索引 0..2 */
    const uint16_t thr3 = (uint16_t)thr * 3u;

    /* 這個 request（也就是「這一段」）的處理後總和 */
    uint64_t dev_sum64 = 0;

    while (sg_idx < qsg->nsg) {
        dma_addr_t cur_addr = qsg->sg[sg_idx].base + sg_byte;
        dma_addr_t cur_len  = qsg->sg[sg_idx].len  - sg_byte;
        uint8_t *buf        = mb + mb_oft;

        dma_memory_read(qsg->as, cur_addr, buf, cur_len, MEMTXATTRS_UNSPECIFIED);

        /* 輕量 debug：每 64 個 SG 印一次 */
        if ((sg_idx % 64) == 0) {
            CSD_debug("[CSD_BMP_BIN][SG%u] len=%llu row_pos=%llu\n",
                      (unsigned)sg_idx,
                      (unsigned long long)cur_len,
                      (unsigned long long)row_pos);
        }

        /* 先做二值化（跨 SG 維持 ch/sum3；遇到 stride 邊界清零） */
        for (uint64_t i = 0; i < (uint64_t)cur_len; i++) {
            if (row_pos < row_bytes) {
                sum3 += buf[i];
                ch++;
                if (ch == 3) {
                    uint8_t v = (sum3 >= thr3) ? 255 : 0;  /* 黑白二值 */
                    buf[i - 2] = v;  /* B */
                    buf[i - 1] = v;  /* G */
                    buf[i]     = v;  /* R */
                    sum3 = 0;
                    ch   = 0;
                }
            }
            row_pos++;
            if (row_pos == stride_ex) { /* 換行：重置像素內狀態 */
                row_pos = 0;
                sum3 = 0;
                ch   = 0;
            }
        }

        /* 就在 buf 內完成後，再把這個 SG 的 bytes 做總和（處理後狀態） */
        uint32_t seg_sum = 0;
        for (uint64_t i = 0; i < (uint64_t)cur_len; i++) {
            seg_sum += buf[i];
        }
        dev_sum64 += seg_sum;

        /* 寫回來賓實體記憶體 */
        dma_memory_write(qsg->as, cur_addr, buf, cur_len, MEMTXATTRS_UNSPECIFIED);

        /* 下一個 SG */
        sg_byte += cur_len;
        if (sg_byte == qsg->sg[sg_idx].len) {
            sg_byte = 0;
            ++sg_idx;
        }
        mb_oft += cur_len;
    }

    /* 把本段的處理後總和（32-bit）回傳到 DW0，供主機端比對 */
    req->cqe.n.result = cpu_to_le32((uint32_t)(dev_sum64 & 0xFFFFFFFFu));
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
        CSD_Filter(n, ns, cmd, req);
        return NVME_SUCCESS;
    case CSD_CMD_Checksum:
        CSD_Checksum(n, ns, cmd, req);
        return NVME_SUCCESS;
    case CSD_CMD_BMP_BIN:
        CSD_BMP_BIN(n, ns, cmd, req);
        return NVME_SUCCESS;

    case CSD_CMD_RESET:
        // 重設統計
        //CSD_debug("\rCSD_CMD_RESET\n");
        csd_total_time_us = 0;
        csd_total_calls = 0;
        csd_total_bytes = 0;
        return NVME_SUCCESS;
    case CSD_CMD_REPORT:
        // 輸出統計資訊
        //CSD_debug("\rCSD_CMD_REPORT\n");
        double avg_us = (double)csd_total_time_us / csd_total_calls;

        femu_log("=== CSD Summary ===\n");
        femu_log("  Total Ops      : %lu\n", csd_total_calls);
        femu_log("  Total Bytes    : %lu\n", csd_total_bytes);
        femu_log("  Total Time     : %lu us\n", csd_total_time_us);
        femu_log("  Avg Time/op    : %.2f us\n", avg_us);
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

