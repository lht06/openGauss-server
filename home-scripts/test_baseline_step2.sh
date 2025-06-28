#!/usr/bin/env bash
set -euo pipefail

echo "==== 第2步：准备大规模测试数据 开始 ===="

# 1）删除旧表
echo "[1/3] 删除旧的 sort_benchmark 表…"
gsql -c "DROP TABLE IF EXISTS sort_benchmark;"

# 2）创建新表（10,000,000 行）
echo "[2/3] 创建 sort_benchmark 表，10,000,000 行…"
gsql -c "CREATE TABLE sort_benchmark AS SELECT generate_series(1,10000000) AS val;"

# 3）校验行数
echo "[3/3] 校验行数…"
count=$(gsql -tA -c "SELECT count(*) FROM sort_benchmark;")
echo "sort_benchmark 行数：$count"
if [[ "$count" -ne 10000000 ]]; then
  echo "❌ Error: 行数不正确"
  exit 1
fi

echo "==== 第2步：大规模测试数据准备 完成 ===="

