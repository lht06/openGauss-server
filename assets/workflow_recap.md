# 测试 & 火焰图生成全流程复现指南

以下是在 Vagrant 虚拟机中，从环境准备到生成火焰图的**详细脚本化步骤**，复制粘贴即可复现：

---

## 前提条件

1. 你已以 `omm` 用户登录 Vagrant 虚拟机。
2. 确保已安装 `git`、`perf`、`gsql`（openGauss 客户端）等基础工具。
3. 确保 OpenGauss 已初始化并运行在单节点模式。

---

## 1. 配置环境变量

使用 `printf`（shell 内置）重写 `~/.bashrc`，恢复常用 PATH 并追加 openGauss 环境变量：

```bash
printf '%s\n' \
  '# 恢复系统命令路径' \
  'export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$HOME/bin' \
  '' \
  '# openGauss 环境变量' \
  'export CODE_BASE=/home/omm/openGauss-base' \
  'export GAUSSHOME=$CODE_BASE/mppdb_temp_install' \
  'export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH' \
  'export PATH=$GAUSSHOME/bin:$PATH' \
  > ~/.bashrc && source ~/.bashrc && echo "$GAUSSHOME"
```

- **预期输出**：
  ```
  /home/omm/openGauss-base/mppdb_temp_install
  ```

---

## 2. 清理操作系统缓存

```bash
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
```

无输出表示清理成功。

---

## 3. 数据库免密登录 & 重启实例

```bash
# 备份并写入免密 pg_hba.conf
cp ~/data/base/pg_hba.conf ~/data/base/pg_hba.conf.bak && \
cat > ~/data/base/pg_hba.conf << 'EOF'
local   all             all                                     trust
host    all             all             127.0.0.1/32            trust
host    all             all             ::1/128                 trust
EOF

# 强制监听 5432 端口并重启
gs_ctl restart \  
  -D ~/data/base \  
  -Z single_node \  
  -o "-p 5432" \  
  -l ~/log/base/openGauss.log | tail -n 2
```

- **请确认** 最后两行输出包含 `server started`。

---

## 4. 简单功能验证

```bash
gsql -d postgres -p 5432 -U omm << 'EOF'
DROP TABLE IF EXISTS demo;
CREATE TABLE demo(id SERIAL PRIMARY KEY, info TEXT);
INSERT INTO demo(info) VALUES ('foo'),('bar');
SELECT id, info FROM demo ORDER BY id;
EOF
```

- **预期输出**：
  ```
   id | info
  ----+------
    1 | foo
    2 | bar
  (2 rows)
  ```

---

## 5. 批量创建测试表

```bash
cat > ~/create_test_tables.sh << 'EOF'
#!/usr/bin/env bash
for SIZE in 2000000 10000000 20000000; do
  echo "==> 创建 demo_${SIZE}"
  gsql -d postgres -p 5432 -U omm << SQL
DROP TABLE IF EXISTS demo_${SIZE};
CREATE TABLE demo_${SIZE} AS SELECT generate_series(1,${SIZE}) AS val;
SQL
done
EOF
chmod +x ~/create_test_tables.sh
~/create_test_tables.sh
```

- **预期输出**：
  ```
  ==> 创建 demo_2000000
  DROP TABLE
  INSERT 0 2000000
  ==> 创建 demo_10000000
  INSERT 0 10000000
  ==> 创建 demo_20000000
  INSERT 0 20000000
  ```

---

## 6. 平均耗时测试脚本

1. **生成脚本**：

   ```bash
   cat > ~/run_avg.sh << 'EOF'
   #!/usr/bin/env bash
   set -euo pipefail
   PORT=$1
   TABLE=$2
   RUNS=${3:-20}
   OUTFILE="${TABLE}_times.csv"
   echo "run,t_ms" > "$OUTFILE"
   SQL="EXPLAIN ANALYZE SELECT * FROM ${TABLE} ORDER BY val LIMIT 100;"
   # 预热
   echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
   gsql -p "$PORT" -U omm -d postgres -c "$SQL" >/dev/null
   # 正式测试
   for i in $(seq 1 $RUNS); do
     echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
     t=$(gsql -p "$PORT" -U omm -d postgres -c "$SQL" | awk '/Total runtime:/ {print $3}')
     echo "${i},${t}" | tee -a "$OUTFILE"
   done
   avg=$(awk -F, 'NR>1{sum+=$2; cnt++} END{printf "%.3f", sum/cnt}' "$OUTFILE")
   echo "Average over $RUNS runs for $TABLE: $avg ms"
   EOF
   chmod +x ~/run_avg.sh
   ```

2. **运行示例**：

   ```bash
   for T in demo_2000000 demo_10000000 demo_20000000; do
     ~/run_avg.sh 5432 $T 20
   done
   ```

- **输出示例**：
  ```
  Average over 20 runs for demo_2000000: 482.143 ms
  Average over 20 runs for demo_10000000: 2417.250 ms
  Average over 20 runs for demo_20000000: 4809.416 ms
  ```

---

## 7. 并发吞吐测试脚本

```bash
cat > ~/run_pgbench.sh << 'EOF'
#!/usr/bin/env bash
set -euo pipefail
PGBENCH=${PG_BENCH:-pgbench}
PORT=$1
SCALE=$2
DURATION=${3:-10}
CLIENTS_LIST=${4:-"1 4 8 16"}
OUTFILE="pgbench_tps.csv"
echo "clients,tps" > "$OUTFILE"
$PGBENCH -p "$PORT" -U omm -d postgres -i -s "$SCALE" >/dev/null
for C in $CLIENTS_LIST; do
  echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  RESULT=$(LC_ALL=C $PGBENCH -p "$PORT" -U omm -d postgres -T "$DURATION" -c "$C" -j "$C" 2>&1 \
    | grep -m1 -Eo 'tps = [0-9]+(\.[0-9]+)?' | awk '{print $3}')
  [ -z "$RESULT" ] && RESULT=0
  echo "${C},${RESULT}" | tee -a "$OUTFILE"
done
EOF
chmod +x ~/run_pgbench.sh

# 运行示例
PG_BENCH=/usr/local/pgsql13/bin/pgbench ~/run_pgbench.sh 5432 5 10 "1 4 8 16"
head -n 6 pgbench_tps.csv
```

- **输出示例**：
  ```
  clients,tps
  1,80.021751
  4,235.226768
  8,314.612121
  16,357.733417
  ```

---

## 8. 火焰图生成脚本

```bash
cat > ~/generate_flamegraph.sh << 'EOF'
#!/usr/bin/env bash
set -euo pipefail
IFS=$'\n\t'
TABLE=${1:-demo_2000000}
DURATION=${2:-30}
# 安装检查
if [ ! -d "$HOME/FlameGraph" ]; then
  git clone https://github.com/brendangregg/FlameGraph.git "$HOME/FlameGraph"
fi
# 清缓存
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
# 后台负载
PIDS=(); for i in $(seq 1 5); do (while true; do gsql -d postgres -p 5432 -U omm -c "SELECT * FROM $TABLE ORDER BY val LIMIT 100;" >/dev/null; done)& PIDS+=("$!"); done
trap 'kill "${PIDS[@]}" >/dev/null 2>&1' EXIT
# 采样
GAUSS_PID=$(pgrep -u "$USER" -f gaussdb | head -n1)
perf record -F 99 -p "$GAUSS_PID" -g -- sleep "$DURATION"
# 生成火焰图
perf script > out.perf
~/FlameGraph/stackcollapse-perf.pl out.perf > out.folded
~/FlameGraph/flamegraph.pl out.folded > flamegraph_${TABLE}.svg
EOF
chmod +x ~/generate_flamegraph.sh

# 运行示例并移动结果到共享目录
~/generate_flamegraph.sh demo_2000000 30
mv flamegraph_demo_2000000.svg /vagrant/
```

- **检查共享目录**：在宿主机项目目录下打开 `flamegraph_demo_2000000.svg`。

---

以上步骤即为**零手动**脚本化操作，全程复制粘贴即可完成环境搭建、基线测试、并发测试和火焰图生成。祝复现顺利！

