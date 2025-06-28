#!/usr/bin/env bash
#
# 简化版一键采集火焰图（系统级采样）
# 前提：已安装 perf、FlameGraph；GAUSSHOME 已设置

# 检查 GAUSSHOME
if [ -z "$GAUSSHOME" ]; then
  echo "错误：请先 export GAUSSHOME=/path/to/openGauss-install"
  exit 1
fi

OUTDIR="$HOME/flame_simple"
mkdir -p "$OUTDIR"
cd "$OUTDIR"

# 1. 启动 gsql 循环查询（后台）
echo "[1] 启动 gsql 循环查询"
"$GAUSSHOME/bin/gsql" -d postgres -p 5432 -U omm \
  -c "while true; do SELECT * FROM demo_2000000 ORDER BY val LIMIT 100; done" \
  >/dev/null 2>&1 &
GSQL_PID=$!
echo "    gsql PID = $GSQL_PID"
sleep 1

# 2. 清理系统缓存
echo "[2] 清理系统缓存"
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

# 3. 系统级 perf 采样 30s
echo "[3] 系统级 perf 采样 30 秒"
sudo perf record -F 300 -a -g -- sleep 30

# 4. 停掉查询
echo "[4] 停止 gsql 查询进程"
kill $GSQL_PID 2>/dev/null || true

# 5. 生成火焰图
echo "[5] 生成火焰图"
sudo perf script > out.perf
~/FlameGraph/stackcollapse-perf.pl out.perf > out.folded
~/FlameGraph/flamegraph.pl out.folded > flamegraph.svg

echo "✅ 完成！火焰图保存在：$OUTDIR/flamegraph.svg"
ls -lh flamegraph.svg
