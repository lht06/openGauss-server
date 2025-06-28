#!/usr/bin/env bash
PORT=$1
RUNS=${2:-20}
TABLES=(demo_2000000 demo_10000000 demo_20000000)

mkdir -p ~/avg_results
cd ~/avg_results

for tbl in "${TABLES[@]}"; do
  echo "==> 正在测试 ${tbl}"
  ~/run_avg.sh "$PORT" "$tbl" "$RUNS"
done
