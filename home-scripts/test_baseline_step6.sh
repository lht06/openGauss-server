#!/usr/bin/env bash
set -euo pipefail

# —— 配置区 ——  
PORT=5432
DATA_DIR=/home/omm
SQL_FILE="$DATA_DIR/sort_test_concurrent.sql"
DURATION=30
CLIENT_COUNTS=(1 2 4 8 16)

# 确保走 /tmp Unix socket，免密
export PGHOST=/tmp
export PGUSER=omm

# 写入并发测试 SQL
cat > "$SQL_FILE" <<SQL
SELECT * FROM sort_benchmark ORDER BY val DESC LIMIT 100;
SQL

echo "==== 第6步：Baseline 并发性能测量 开始 ===="
echo "测试持续时间：${DURATION}s；测试 SQL：${SQL_FILE}"
echo

# 遍历并发客户端数并输出结果
for c in "${CLIENT_COUNTS[@]}"; do
  echo "-- 并发客户端: $c --"
  pgbench -n \
    -p $PORT \
    -U $PGUSER \
    -T $DURATION \
    -c $c \
    -r \
    -f "$SQL_FILE" \
    | awk '/latency average/ { printf "  平均延迟: %s ms\n", $3 }
           /excluding/        { printf "  TPS: %s\n", $3 }'
  echo
done

# 清理
rm -f "$SQL_FILE"

echo "==== 第6步 完成 ===="
