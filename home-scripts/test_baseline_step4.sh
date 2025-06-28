#!/usr/bin/env bash
set -euo pipefail

FOLDED=perf.folded
SVG=flamegraph_baseline.svg

echo "==== 第4步：分析火焰图 开始 ===="
echo
echo "1) 火焰图文件路径：$(pwd)/$SVG"
echo "   → 可用浏览器打开查看："
echo "     scp $USER@$HOSTNAME:$(pwd)/$SVG ~/Downloads/"
echo "     xdg-open ~/Downloads/$SVG    # 或 macOS: open ~/Downloads/$SVG"
echo
echo "2) 从 $FOLDED 提取前10热调用栈："
awk -F ' ' '{counts[$1]+=$2} END { for (s in counts) print counts[s], s }' "$FOLDED" \
  | sort -nr | head -n 10
echo
echo "==== 第4步：分析火焰图 完成 ===="
