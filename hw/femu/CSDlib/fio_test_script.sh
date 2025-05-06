#!/bin/bash
# FIO 測試腳本 - 自動執行多種測試並輸出結果

OUTPUT_DIR="fio_results"
rm -rf "$OUTPUT_DIR"  # 清除舊資料避免多餘資料產生
mkdir -p "$OUTPUT_DIR"

TEST_DIR=${TEST_DIR:-$(pwd)}
SIZE="2G"
RUNTIME="60"
JOBS=4

tests=(
  "--rw=randread --bs=4k"
  "--rw=randwrite --bs=4k"
  "--rw=randrw --bs=4k"
  "--rw=read --bs=1M"
  "--rw=write --bs=1M"
)

for i in "${!tests[@]}"; do
  test_params=${tests[$i]}
  test_name="test_$(($i+1))"
  echo "Running $test_name with params: $test_params"

  fio --name="$test_name" \
      --directory="$TEST_DIR" \
      --ioengine=libaio \
      $test_params \
      --size="$SIZE" \
      --runtime="$RUNTIME" \
      --time_based \
      --numjobs="$JOBS" \
      --group_reporting \
      > "$OUTPUT_DIR/${test_name}.log"
done

echo "All tests completed. Results saved in $OUTPUT_DIR."