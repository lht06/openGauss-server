#!/usr/bin/env bash
set -euo pipefail

# —— 配置区 ——  
BASE_DATA_DIR=/home/omm/data/base
MOD_DATA_DIR=/home/omm/data/mod
MOD_LOG_DIR=/home/omm/log/mod
MOD_PORT=5433
DATA_DIR=/home/omm
SQL_FILE="${DATA_DIR}/sort_test_mod.sql"
DURATION=30
CLIENTS=1
PGBENCH=/usr/bin/pgbench

# 确保走 /tmp Unix socket，免密
export PGHOST=/tmp
export PGUSER=omm
export LD_LIBRARY_PATH=

echo "==== 第6步：切换到修改版并进行排序查询性能测量（修正版） 开始 ===="

# 0）给修改版实例配置 trust（本地免密码）
echo "[0/4] 配置修改版 pg_hba.conf 为 trust…"
MOD_HBA="$MOD_DATA_DIR/pg_hba.conf"
cp "$MOD_HBA" "${MOD_HBA}.bak"
cat > /tmp/pg_tmp_hba << 'HBA'
local   all             all                                     trust
host    all             all             127.0.0.1/32            trust
host    all             all             ::1/128                 trust

HBA
cat /tmp/pg_tmp_hba "$MOD_HBA" > "${MOD_HBA}.new"
mv "${MOD_HBA}.new" "$MOD_HBA"
rm /tmp/pg_tmp_hba

# 1）停止 Baseline，启动修改版实例
echo "[1/4] 停止 Baseline 并启动修改版（port $MOD_PORT）…"
gs_ctl stop -D "$BASE_DATA_DIR" || true
sleep 3
gs_ctl start -D "$MOD_DATA_DIR" -Z single_node \
  -o "-p $MOD_PORT -k /tmp" \
  -l "$MOD_LOG_DIR/openGauss.log"
sleep 5
echo "✅ 修改版实例已启动"

# 2）创建测试表（10M 行）
echo "[2/4] 创建 sort_benchmark 表（10,000,000 行）…"
gsql -p "$MOD_PORT" -c "DROP TABLE IF EXISTS sort_benchmark;"
gsql -p "$MOD_PORT" -c "CREATE TABLE sort_benchmark AS SELECT generate_series(1,10000000) AS val;"

# 3）写入排序查询脚本
echo "[3/4] 写入排序测试脚本 $SQL_FILE …"
cat > "$SQL_FILE" <<EOSQL
-- sort_test_mod.sql
SELECT * FROM sort_benchmark ORDER BY val DESC LIMIT 100;
EOSQL

# 4）运行 pgbench 性能测量
echo "[4/4] 用 pgbench 运行 ${DURATION}s，${CLIENTS} 客户端…"
${PGBENCH} -n -p "$MOD_PORT" -U "$PGUSER" \
  -T "$DURATION" -c "$CLIENTS" -r \
  -f "$SQL_FILE"

# 清理
rm -f "$SQL_FILE"

echo "==== 第6步 完成：修改版性能测量结束 ===="
