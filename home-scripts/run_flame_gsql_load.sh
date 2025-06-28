#!/usr/bin/env bash
#
# 用 gsql 并行循环查询制造高负载并采样火焰图
# 依赖：perf、FlameGraph；GAUSSHOME 已指向 openGauss 安装目录
# 你可以自定义 CONNS（并发 gsql 个数）和 DURATION（秒）

CONNS=${1:-16}       # 并发 gsql 进程数
DURATION=${2:-60}    # 采样时长（秒）

OUTDIR="$HOME/flame_gsql_load"
mkdir -p "$OUTDIR"
cd "$OUTDIR"

echo "[1] 清理系统缓存"
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

echo "[2] 启动 $CONNS 个 gsql 背景查询进程（$DURATION 秒）"
for i in $(seq 1 $CONNS); do
  LD_LIBRARY_PATH="$GAUSSHOME/lib:/usr/lib64" \
    "$GAUSSHOME/bin/gsql" -d postgres -p 5432 -U omm \
      -c "while true; do SELECT * FROM demo_2000000 ORDER BY val LIMIT 100; done" \
      >/dev/null 2>&1 &
  PIDS[$i]=$!
done
echo "   PIDs: ${PIDS[*]}"
sleep 1

echo "[3] 系统级 perf 采样 $DURATION 秒"
sudo perf record -F 300 -a -g -- sleep $DURATION

echo "[4] 停止所有 gsql 查询进程"
for pid in "${PIDS[@]}"; do
  kill "$pid" 2>/dev/null
done

echo "[5] 生成火焰图"
sudo perf script > out.perf
~/FlameGraph/stackcollapse-perf.pl out.perf > out.folded
~/FlameGraph/flamegraph.pl out.folded > flamegraph.svg

echo "✅ 完成！火焰图保存在：$OUTDIR/flamegraph.svg"
ls -lh flamegraph.svg
