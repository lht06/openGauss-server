#!/usr/bin/env bash
set -euo pipefail

# —— 配置区 ——  
PORT=5432                       # Baseline 默认端口  
DURATION=20                     # 采样时长（秒）  
FG_DIR=/home/omm/FlameGraph      # FlameGraph 脚本所在目录  

echo "==== 第3步：性能采样并生成火焰图（修正版 v3） 开始 ===="

# 1）获取 gaussdb PID
echo "[1/6] 获取 Baseline gaussdb PID…"
pid=$(pgrep -u "$(whoami)" -f "gaussdb.*-p $PORT")
if [[ -z "$pid" ]]; then
  echo "❌ Error: 无法找到 gaussdb 进程（端口 $PORT）"
  exit 1
fi
echo "✅ Found PID: $pid"

# 2）清空文件系统缓存
echo "[2/6] 清空系统文件缓存…"
sudo /usr/local/bin/drop_caches.sh

# 3）启动 perf 录制
echo "[3/6] 开始 perf 录制 ($DURATION 秒)…"
sudo perf record -F 300 -p "$pid" -g -- sleep "$DURATION" &
perf_pid=$!

# 4）等待 perf 就绪
sleep 1

# 5）顺序执行排序测试 5 次
echo "[4/6] 顺序执行排序测试 5 次…"
for i in {1..5}; do
  gsql -P pager=off -c "SELECT * FROM sort_benchmark ORDER BY val DESC LIMIT 100;"
done

# 6）等待 perf 录制结束
echo "[5/6] 等待 perf 录制结束…"
wait $perf_pid

# 7）修复 perf.data 所有权并生成火焰图
echo "[6/6] 修复 perf.data 所有权并生成火焰图…"
sudo chown $(whoami) perf.data
perf script > perf.out
"$FG_DIR/stackcollapse-perf.pl" perf.out > perf.folded
"$FG_DIR/flamegraph.pl" perf.folded > flamegraph_baseline.svg

echo "==== 第3步 完成：火焰图已生成 → $(pwd)/flamegraph_baseline.svg ===="
