#!/bin/bash

# ==============================================
# gen_test_dataB.sh
# 產生可完整填滿 BLOCK_SIZE 的 TEST_dataB 測試檔
# Payload: Payload123ABCabc台灣CSD測試資料456defDEF
# 適用 FEMU 模擬平台測試 Filter / Checksum / DMA
# ==============================================

FILENAME="TEST_data_ascii_mixed"
BLOCK_SIZE=524288  # 預設 512KB

echo "[INFO] Generating $FILENAME with size ${BLOCK_SIZE} bytes..."

PATTERN="Hello123ABCabc台灣CSD測試資料456defDEF🙂END"
PATTERN_LEN=${#PATTERN}

# 使用 Python 快速產生填滿完整大小的檔案
python3 -c "
pattern = '$PATTERN'.encode('utf-8')
with open('$FILENAME', 'wb') as f:
    repeat_times = $BLOCK_SIZE // len(pattern) + 1
    data = pattern * repeat_times
    f.write(data[:$BLOCK_SIZE])
"

echo "[INFO] $FILENAME generated successfully."
ls -lh $FILENAME
