#!/usr/bin/env bash
#
# 一键高负载采集火焰图脚本
# 依赖：perf、FlameGraph；GAUSSHOME 已指向 openGauss 安装目录
# 强制走 TCP 连接，避免 socket 问题

PG_BENCH=${PG_BENCH:-pgbench}
PORT=${1:-5432}
SCALE=${2:-10}
CLIENTS=${3:-16}
THREADS=${4:-4}
DURATION=${5:-60}

OUTDIR="$HOME/flame_load"
mkdir -p "$OUTDIR"
cd "$OUTDIR"

echo "[1] 清理系统缓存"
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

echo "[2] 高负载：pgbench -h 127.0.0.1 -p $PORT -U omm -c $CLIENTS -j $THREADS -s $SCALE -T $DURATION"
echo "    使用 ${PG_BENCH}"
sudo perf record -F 300 -a -g -- timeout $DURATION \
  $PG_BENCH -h 127.0.0.1 -p $PORT -U omm -d postgres \
    -c $CLIENTS -j $THREADS -s $SCALE -T $DURATION

echo "[3] 生成火焰图"
sudo perf script > out.perf
~/FlameGraph/stackcollapse-perf.pl out.perf > out.folded
~/FlameGraph/flamegraph.pl out.folded > flamegraph.svg

echo "✅ 完成！高负载火焰图保存在：$OUTDIR/flamegraph.svg"
ls -lh flamegraph.svg
