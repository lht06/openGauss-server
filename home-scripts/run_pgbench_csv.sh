#!/usr/bin/env bash
# 保存为 ~/run_pgbench_csv.sh
PORT=$1
SCALE=$2
OUTFILE=$3

if [[ -z "$PORT" || -z "$SCALE" || -z "$OUTFILE" ]]; then
  echo "Usage: $0 <PORT> <SCALE> <OUTFILE.csv>"
  exit 1
fi

# CSV 头
echo "clients,tps" > "$OUTFILE"

USER=omm
DB=postgres

# 初始化 pgbench
pgbench -p "$PORT" -U "$USER" -d "$DB" -i -s "$SCALE" > /dev/null

# 并发测试：clients 取 1,4,8,16
for C in 1 4 8 16; do
  # 清缓存
  echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  # 执行 10 秒测试
  tps=$(PGHOST=/tmp LD_LIBRARY_PATH= pgbench -p "$PORT" -U "$USER" -d "$DB" \
    -T 10 -c "$C" -j "$C" 2>/dev/null \
    | awk '/excluding connections establishing/ {print $3; exit}')

  echo "${C},${tps}" >> "$OUTFILE"
  echo "  clients=${C}: ${tps} tps"
done

echo "Results written to $OUTFILE"

