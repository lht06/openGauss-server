#!/usr/bin/env bash
# 用法: ./run_avg.sh <PORT> <TABLE> [RUNS]
PORT=$1
TABLE=$2
RUNS=${3:-20}
OUTFILE="${TABLE}_times.csv"

echo "run,t_ms" > "$OUTFILE"

SQL="EXPLAIN ANALYZE SELECT * FROM ${TABLE} ORDER BY val LIMIT 100;"

# 预热
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
gsql -p "$PORT" -U omm -d postgres -c "$SQL" >/dev/null

# 正式测试
for i in $(seq 1 $RUNS); do
  echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  t=$(gsql -p "$PORT" -U omm -d postgres -c "$SQL" \
      | awk '/Total runtime:/ {print $3}')
  echo "${i},${t}" | tee -a "$OUTFILE"
done

# 计算并打印平均值
avg=$(awk -F, 'NR>1{sum+=$2; cnt++} END{printf "%.3f", sum/cnt}' "$OUTFILE")
echo "Average over $RUNS runs for $TABLE: $avg ms"
