#!/usr/bin/env bash
#
# 一键重负载采集火焰图（demo_20000000 全表排序）
# GAUSSHOME 已指向 openGauss 安装目录
CONNS=${1:-32}       # 并发 gsql 数
DURATION=${2:-90}    # 采样时长（秒）
TABLE=${3:-demo_20000000}

OUTDIR="$HOME/flame_heavy"
mkdir -p "$OUTDIR"
cd "$OUTDIR"

echo "[1] 清理系统缓存"
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

echo "[2] 启动 $CONNS 个 gsql 循环，执行 SELECT * FROM $TABLE ORDER BY val"
for i in $(seq 1 $CONNS); do
  LD_LIBRARY_PATH="$GAUSSHOME/lib:/usr/lib64" \
    "$GAUSSHOME/bin/gsql" -d postgres -p 5432 -U omm \
      -c "while true; do SELECT * FROM $TABLE ORDER BY val; done" \
      >/dev/null 2>&1 &
  PIDS[$i]=$!
done
echo "    PIDs: ${PIDS[*]}"
sleep 2

echo "[3] 系统级 perf 采样 $DURATION 秒"
sudo perf record -F 300 -a -g -- sleep $DURATION

echo "[4] 停止所有 gsql 进程"
for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done

echo "[5] 生成火焰图"
sudo perf script > out.perf
~/FlameGraph/stackcollapse-perf.pl out.perf > out.folded
~/FlameGraph/flamegraph.pl out.folded > flamegraph.svg

echo "✅ 完成！火焰图保存在：$OUTDIR/flamegraph.svg"
ls -lh flamegraph.svg
