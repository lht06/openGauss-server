#!/usr/bin/env bash
set -euo pipefail

# —— 配置区 ——  
PORT=5432
DATA_DIR=/home/omm
SQL_FILE="${DATA_DIR}/sort_test.sql"
DURATION=30
CLIENTS=1
PGBENCH=/usr/bin/pgbench

# 确保走 /tmp Unix socket
export PGHOST=/tmp
export PGUSER=omm

echo "==== 第5步：基线排序查询性能测量 开始 ===="
echo "[1/4] 写入排序测试脚本 ${SQL_FILE} …"
cat > "${SQL_FILE}" <<EOSQL
-- sort_test.sql
SELECT * FROM sort_benchmark ORDER BY val DESC LIMIT 100;
EOSQL

echo "[2/4] 预热查询 1 次 …"
gsql -P pager=off -c "\i ${SQL_FILE}" >/dev/null

echo "[3/4] 用 pgbench 运行 ${DURATION} 秒，${CLIENTS} 客户端 …"
# 清空 LD_LIBRARY_PATH 以防库冲突
LD_LIBRARY_PATH= ${PGBENCH} -n \
  -p ${PORT} -U ${PGUSER} \
  -T ${DURATION} -c ${CLIENTS} -r \
  -f "${SQL_FILE}"

echo "[4/4] 清理测试脚本 …"
rm -f "${SQL_FILE}"

echo "==== 第5步 完成 ===="
