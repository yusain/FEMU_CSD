#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
cd -- "$(dirname "$0")"

OUTDIR="EXE"
DATA_DIR="DataSet/Filter_DataSet"
DNA_DIR="$DATA_DIR/dna_chunks"
mkdir -p "$OUTDIR" "$DATA_DIR" "$DNA_DIR"

# 粒度
SZ_10M=$((10*1024*1024))
SZ_1M=$((1*1024*1024))
SZ_4K=$((4*1024))

# ========= 0) 清除舊輸出 =========
rm -f TEST_ASCII_OUT_*.txt TEST_DNA_OUT_*.txt /tmp/bit_*.log 2>/dev/null || true

# ========= 小工具 =========
with_fifo() {  # 用 FIFO 串流，不落地子檔
  local producer_cmd="$1"
  local consumer_cmd="$2"
  local fifo; fifo="$(mktemp -u /tmp/csd_fifo.XXXXXX)"
  mkfifo "$fifo"
  ( eval "${producer_cmd} > \"${fifo}\"" ) &        # producer 背景寫 FIFO
  local prod_pid=$!
  eval "${consumer_cmd/@@FIFO@@/${fifo}}"           # consumer 讀 FIFO（其輸出會回到呼叫端）
  wait "${prod_pid}"
  rm -f "${fifo}"
}

dna_stream_producer() {  # 「整行對齊 + N 補齊」的 FASTA 串流
  local base="$1"; local size="$2"
  printf "python3 -c 'import sys;src=sys.argv[1];goal=int(sys.argv[2]);w=0\n"
  printf "f=open(src,\"rb\")\n"
  printf "for line in f:\n L=len(line)\n if w>0 and w+L>goal: break\n sys.stdout.buffer.write(line); w+=L\n"
  printf "f.close()\nif w<goal: sys.stdout.buffer.write(b\"N\"*(goal-w))' '%s' '%s'" "$base" "$size"
}

# 產 ASCII '0'/'1' 隨機流（避免 popcount 誤解）
bits_stream_producer_ascii() {
  local size="$1"
  printf "python3 -c 'import sys,os;n=int(sys.argv[1]);"
  printf "b=os.urandom(n);sys.stdout.buffer.write(bytes(((x&1)+48) for x in b))' '%s'" "$size"
}

# ========= 1) 準備測試資料 =========
echo "== 1) 準備測試資料（BIT/ASCII 10MiB 基底；DNA 切 10 份 × 10MiB） =="

# 1A) BIT 10MiB '01' 基底
BIT_BASE="$DATA_DIR/TEST_BIT_PATTERN_10M.bin"
if [[ ! -s "$BIT_BASE" || $(stat -c%s "$BIT_BASE") -ne $SZ_10M ]]; then
  python3 -c 'import sys;p=sys.argv[1];n=int(sys.argv[2]);pat=b"01";q,r=divmod(n,len(pat));open(p,"wb").write(pat*q+pat[:r])' \
           "$BIT_BASE" "$SZ_10M"
fi
echo "[OK] $BIT_BASE ($(stat -c%s "$BIT_BASE") B)"

# 1B) ASCII 10MiB 基底
ASCII_PATTERN="Hello123 ABCabc CSD測試資料 456defDEF🙂"
ASCII_BASE="$DATA_DIR/TEST_ASCII_PATTERN_10M.txt"
if [[ ! -s "$ASCII_BASE" || $(stat -c%s "$ASCII_BASE") -ne $SZ_10M ]]; then
  python3 -c 'import sys;p=sys.argv[1];n=int(sys.argv[2]);pat=sys.argv[3].encode();q,r=divmod(n,len(pat));open(p,"wb").write(pat*q+pat[:r])' \
           "$ASCII_BASE" "$SZ_10M" "$ASCII_PATTERN"
fi
echo "[OK] $ASCII_BASE ($(stat -c%s "$ASCII_BASE") B)"

# 1C) DNA：切成恰好 10 份 × 10MiB（不足用 N 補）
FASTA_SRC="$DATA_DIR/ERR015526_2.fasta"
if [[ ! -f "$FASTA_SRC" && -f "ERR015526_2.fasta" ]]; then
  echo "[INFO] moving ERR015526_2.fasta -> $DATA_DIR/"
  mv -f ERR015526_2.fasta "$DATA_DIR/"
fi

split_fasta_to_10_exact() {
  local fasta="$1"; local outdir="$2"; local size="$3"
  rm -f "$outdir"/dna_chunk_*
  if [[ -f "$fasta" ]]; then
    awk -v chunk="$size" -v out="$outdir/dna_chunk_" '
      BEGIN { i=0; sz=0; fn=sprintf("%s%02d", out, i) }
      {
        L = length($0) + 1
        if (sz>0 && sz+L>chunk) {
          close(fn); i++; if (i>=10) exit
          sz=0; fn=sprintf("%s%02d", out, i)
        }
        print $0 >> fn; sz += L
      }
      END { close(fn) }
    ' "$fasta"
  fi
  local cnt=0
  for f in "$outdir"/dna_chunk_*; do
    [[ -e "$f" ]] || continue
    cnt=$((cnt+1))
    local cur; cur=$(stat -c%s "$f")
    if (( cur < size )); then
      python3 -c 'import sys; p=sys.argv[1]; need=int(sys.argv[2]); open(p,"ab").write(b"N"*need)' \
               "$f" $((size-cur))
    fi
  done
  while (( cnt < 10 )); do
    local idx; idx=$(printf "%02d" "$cnt")
    python3 -c 'import sys; p=sys.argv[1]; n=int(sys.argv[2]); open(p,"wb").write(b"N"*n)' \
             "$outdir/dna_chunk_$idx" "$size"
    cnt=$((cnt+1))
  done
}

if [[ -f "$FASTA_SRC" ]]; then
  split_fasta_to_10_exact "$FASTA_SRC" "$DNA_DIR" "$SZ_10M"
  echo "[OK] DNA chunks under $DNA_DIR (10 files x 10MiB)"
  ls -lh "$DNA_DIR"/dna_chunk_* | head
else
  echo "[WARN] $FASTA_SRC not found; DNA 測試將略過"
fi

# ========= 2) 編譯 =========
echo "== 2) 編譯（Makefile） =="
make -j

# ========= 3) 執行測試（BIT 只印最後統計；ASCII/DNA 先印 kept= 再接最後統計） =========
echo "== 3) 執行測試 =="

# ---- BIT ----
echo "-- BIT --"

run_bit_file() {
  local path="$1"; local tag="$2"
  local last; last="$(sudo "$OUTDIR/filter_bitcount_test" "$path" | tail -n1)"
  printf "BIT   %-14s | %s\n" "$tag" "$last"
}
run_bit_stream() {
  local size="$1"; local tag="$2"; local producer_cmd="$3"
  local last; last="$(with_fifo "$producer_cmd" "sudo \"$OUTDIR/filter_bitcount_test\" @@FIFO@@ | tail -n1")"
  printf "BIT   %-14s | %s\n" "$tag" "$last"
}

run_bit_file   "$BIT_BASE" "PATTERN_10M"
run_bit_stream "$SZ_1M"    "PATTERN_1M" "$(printf 'head -c %d \"%s\"' "$SZ_1M" "$BIT_BASE")"
run_bit_stream "$SZ_4K"    "PATTERN_4K" "$(printf 'head -c %d \"%s\"' "$SZ_4K" "$BIT_BASE")"
run_bit_stream "$SZ_10M"   "URND_10M"   "$(bits_stream_producer_ascii "$SZ_10M")"
run_bit_stream "$SZ_1M"    "URND_1M"    "$(bits_stream_producer_ascii "$SZ_1M")"
run_bit_stream "$SZ_4K"    "URND_4K"    "$(bits_stream_producer_ascii "$SZ_4K")"

# ---- ASCII ----
echo "-- ASCII --"

run_ascii_file() {
  local path="$1"; local tag="$2"
  local out="TEST_ASCII_OUT_${tag}.txt"
  local inb; inb=$(stat -c%s "$path")
  local last; last="$(sudo "$OUTDIR/filter_ascii_test" "$path" "$out" | tail -n1)"
  local outb; outb=$(stat -c%s "$out")
  local kept; kept=$(awk -v a="$outb" -v b="$inb" 'BEGIN{printf "%.1f", (b>0? 100*a/b : 0)}')
  printf "ASCII %-14s kept=%4s%% | %s\n" "$tag" "$kept" "$last"
}
run_ascii_stream() {
  local size="$1"; local tag="$2"; local producer_cmd="$3"
  local out="TEST_ASCII_OUT_${tag}.txt"
  local last; last="$(with_fifo "$producer_cmd" "sudo \"$OUTDIR/filter_ascii_test\" @@FIFO@@ \"$out\" | tail -n1")"
  local outb; outb=$(stat -c%s "$out")
  local kept; kept=$(awk -v a="$outb" -v b="$size" 'BEGIN{printf "%.1f", (b>0? 100*a/b : 0)}')
  printf "ASCII %-14s kept=%4s%% | %s\n" "$tag" "$kept" "$last"
}

run_ascii_file   "$ASCII_BASE" "PATTERN_10M"
run_ascii_stream "$SZ_1M"      "PATTERN_1M" "$(printf 'head -c %d \"%s\"' "$SZ_1M" "$ASCII_BASE")"
run_ascii_stream "$SZ_4K"      "PATTERN_4K" "$(printf 'head -c %d \"%s\"' "$SZ_4K" "$ASCII_BASE")"
run_ascii_stream "$SZ_10M"     "URND_10M"   "head -c $SZ_10M /dev/urandom"
run_ascii_stream "$SZ_1M"      "URND_1M"    "head -c $SZ_1M /dev/urandom"
run_ascii_stream "$SZ_4K"      "URND_4K"    "head -c $SZ_4K /dev/urandom"

# ---- DNA ----
if compgen -G "$DNA_DIR/dna_chunk_*" > /dev/null; then
  echo "-- DNA (pick 2 of 10) --"

  run_dna_file() {
    local path="$1"; local tag="$2"
    local out="TEST_DNA_OUT_${tag}.txt"
    local inb; inb=$(stat -c%s "$path")
    local last; last="$(sudo "$OUTDIR/filter_dna_test" "$path" "$out" | tail -n1)"
    local outb; outb=$(stat -c%s "$out")
    local kept; kept=$(awk -v a="$outb" -v b="$inb" 'BEGIN{printf "%.1f", (b>0? 100*a/b : 0)}')
    printf "DNA   %-14s kept=%4s%% | %s\n" "$tag" "$kept" "$last"
  }
  run_dna_stream() {
    local base="$1"; local size="$2"; local tag="$3"
    local out="TEST_DNA_OUT_${tag}.txt"
    local prod; prod="$(dna_stream_producer "$base" "$size")"
    local last; last="$(with_fifo "$prod" "sudo \"$OUTDIR/filter_dna_test\" @@FIFO@@ \"$out\" | tail -n1")"
    local outb; outb=$(stat -c%s "$out")
    local kept; kept=$(awk -v a="$outb" -v b="$size" 'BEGIN{printf "%.1f", (b>0? 100*a/b : 0)}')
    printf "DNA   %-14s kept=%4s%% | %s\n" "$tag" "$kept" "$last"
  }

  mapfile -t picked < <(ls "$DNA_DIR"/dna_chunk_* | sort | shuf -n 2)
  for f in "${picked[@]}"; do
    bn=$(basename "$f"); tag=${bn#dna_chunk_}
    run_dna_file   "$f"               "CHUNK_${tag}_10M"
    run_dna_stream "$f" "$SZ_1M"      "CHUNK_${tag}_1M"
    run_dna_stream "$f" "$SZ_4K"      "CHUNK_${tag}_4K"
  done
else
  echo "-- DNA --"
  echo "[SKIP] DNA chunks not found in $DNA_DIR"
fi

echo "== Done =="
