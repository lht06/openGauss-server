#!/usr/bin/env bash
# 用法：PG_BENCH=/path/to/pgbench ./run_pgbench.sh <PORT> <SCALE> <DURATION> <CLIENTS_LIST>
PGBENCH=${PG_BENCH:-pgbench}
PORT=$1
SCALE=$2
DURATION=${3:-10}
CLIENTS_LIST=${4:-"1 4 8 16"}

OUTFILE="pgbench_tps.csv"
echo "clients,tps" > "$OUTFILE"

echo "==> 初始化 pgbench，scale=$SCALE"
$PGBENCH -p "$PORT" -U omm -d postgres -i -s "$SCALE" >/dev/null

for C in $CLIENTS_LIST; do
  echo "==> 测试并发 clients=$C"
  echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

  RESULT=$(
    LC_ALL=C $PGBENCH -p "$PORT" -U omm -d postgres \
      -T "$DURATION" -c "$C" -j "$C" 2>&1 \
      | grep -m1 -Eo 'tps = [0-9]+(\.[0-9]+)?' \
      | awk '{print $3}'
  )
  [ -z "$RESULT" ] && RESULT=0

  echo "${C},${RESULT}" | tee -a "$OUTFILE"
done

echo "测试完成，结果保存在 $(pwd)/$OUTFILE"
