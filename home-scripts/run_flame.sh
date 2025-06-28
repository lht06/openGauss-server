#!/usr/bin/env bash
#
# 一键化采集 OpenGauss gsql 的火焰图
# 要求：已安装 perf、FlameGraph；环境变量 GAUSSHOME 已指向 openGauss 安装目录

# 1. 检查 GAUSSHOME
if [ -z "$GAUSSHOME" ]; then
  echo "错误：GAUSSHOME 未设置，请先 export GAUSSHOME=/path/to/openGauss"
  exit 1
fi

OUTDIR="$HOME/flame_out"
mkdir -p "$OUTDIR"

# 2. 启动循环查询
echo "[1] 启动 gsql 循环查询（后台）"
"$GAUSSHOME/bin/gsql" -d postgres -p 5432 -U omm \
  -c "while true; do SELECT * FROM demo_2000000 ORDER BY val LIMIT 100; done" \
  >/dev/null 2>&1 &
GSQL_PID=$!
echo "    gsql PID = $GSQL_PID"
sleep 1

# 3. 清空缓存
echo "[2] 清理系统缓存"
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

# 4. 采样 30 秒（仅针对 gsql 进程）
echo "[3] 对 PID=$GSQL_PID 采样 30s"
sudo perf record -F 300 -g -p $GSQL_PID -o "$OUTDIR/perf.data" -- sleep 30

# 5. 停掉查询
echo "[4] 停止 gsql 进程"
kill $GSQL_PID 2>/dev/null || true

# 6. 生成火焰图
echo "[5] 生成火焰图"
sudo perf script -i "$OUTDIR/perf.data" > "$OUTDIR/out.perf"
~/FlameGraph/stackcollapse-perf.pl "$OUTDIR/out.perf" > "$OUTDIR/out.folded"
~/FlameGraph/flamegraph.pl "$OUTDIR/out.folded" > "$OUTDIR/flamegraph.filtered.svg"

echo "✅ 完成！火焰图保存在：$OUTDIR/flamegraph.filtered.svg"
ls -lh "$OUTDIR/flamegraph.filtered.svg"
