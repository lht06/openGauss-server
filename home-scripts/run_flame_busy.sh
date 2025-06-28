#!/usr/bin/env bash
#
# 一键 busywork 火焰图采样脚本
# 依赖：perf、FlameGraph；GAUSSHOME 已指向 openGauss 安装目录
# 可传入并发数和采样时长：./run_flame_busy.sh [CONNS] [DURATION]
# 默认 CONNS=16, DURATION=90

CONNS=\${1:-16}
DURATION=\${2:-90}

# 1. 在数据库中创建 busywork 函数
echo "[0] 在数据库中创建 busywork() 函数"
\$GAUSSHOME/bin/gsql -d postgres -p 5432 -U omm <<SQL
CREATE OR REPLACE FUNCTION busywork(n int) RETURNS void AS \$\$
DECLARE i int; x double precision := 0;
BEGIN
  FOR i IN 1..\$1 LOOP
    x := x + sin(i) * cos(i);
  END LOOP;
END;
\$\$ LANGUAGE plpgsql;
SQL

OUTDIR="\$HOME/flame_busy"
mkdir -p "\$OUTDIR"
cd "\$OUTDIR"

# 2. 清理系统缓存
echo "[1] 清理系统缓存"
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

# 3. 并行启动 CONNS 个 gsql 调用 busywork(1000000)
echo "[2] 启动 \$CONNS 个 gsql 并行 busywork(1000000)"
for i in \$(seq 1 \$CONNS); do
  LD_LIBRARY_PATH="\$GAUSSHOME/lib:/usr/lib64" \
    "\$GAUSSHOME/bin/gsql" -d postgres -p 5432 -U omm \
      -c "while true; do SELECT busywork(1000000); done" \
      >/dev/null 2>&1 &
  PIDS[\$i]=\$!
done
echo "   PIDs: \${PIDS[*]}"
sleep 2

# 4. 系统级 perf 采样 DURATION 秒
echo "[3] perf 采样 \$DURATION 秒"
sudo perf record -F 300 -a -g -- sleep \$DURATION

# 5. 停掉所有 gsql 进程
echo "[4] 停止所有 gsql 进程"
for pid in "\${PIDS[@]}"; do kill \$pid 2>/dev/null; done

# 6. 折叠并生成火焰图
echo "[5] 生成火焰图"
sudo perf script > out.perf
~/FlameGraph/stackcollapse-perf.pl out.perf > out.folded
~/FlameGraph/flamegraph.pl out.folded > flamegraph.busy.svg

echo "✅ 完成！火焰图保存在：\$OUTDIR/flamegraph.busy.svg"
ls -lh flamegraph.busy.svg
