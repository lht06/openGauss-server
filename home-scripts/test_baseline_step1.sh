#!/usr/bin/env bash
set -euo pipefail

# —— 配置区 ——  
DATA_DIR=/home/omm/data/base  
LOG_DIR=/home/omm/log/base  
PORT=5432            # baseline 默认端口  
FG_DIR=/home/omm/FlameGraph  

echo "==== 第1步：Baseline环境验证 开始 ===="

# 1）启动 Baseline 实例并获取 PID
echo "[1/4] 启动 Baseline 实例…"
gs_ctl start -D $DATA_DIR -Z single_node -l $LOG_DIR/openGauss.log
sleep 5
pid=$(pgrep -u $(whoami) -f "gaussdb.*-p $PORT")
if [[ -z "$pid" ]]; then
  echo "❌ Error: 未找到 gaussdb 进程（端口 $PORT）"
  exit 1
fi
echo "✅ gaussdb PID: $pid"

# 2）perf 与 FlameGraph 脚本检查
echo "[2/4] 检查 perf 与 FlameGraph 脚本…"
perf --version
ls $FG_DIR/stackcollapse-perf.pl $FG_DIR/flamegraph.pl

# 3）清缓存（可选，但推荐）
echo "[3/4] 清空系统文件缓存…"
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

# 4）建表并校验 demo 表行数
echo "[4/4] 建表并校验 demo 表行数…"
gsql -d postgres -p $PORT -U omm <<EOF
DROP TABLE IF EXISTS demo;
CREATE TABLE demo AS SELECT generate_series(1,10000) AS val;
EOF
count=$(gsql -d postgres -p $PORT -U omm -tA -c "SELECT count(*) FROM demo;")
echo "demo 表行数：$count"
if [[ "$count" -ne 10000 ]]; then
  echo "❌ Error: demo 表行数不正确"
  exit 1
fi

echo "==== 第1步：Baseline环境验证 完成 ===="

