
# are authenticated, which PostgreSQL user names they can use, which
PORT=$1
TABLE=$2
OUTFILE=$3

if [[ -z "$PORT" || -z "$TABLE" || -z "$OUTFILE" ]]; then
  echo "Usage: $0 <PORT> <TABLE> <OUTFILE.csv>"
  exit 1
fi

# 写入 CSV 头
echo "table,run,ms" > "$OUTFILE"

SQL="EXPLAIN ANALYZE SELECT * FROM ${TABLE} ORDER BY val LIMIT 100;"

# 预热一次
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
gsql -p "$PORT" -U omm -d postgres -c "$SQL" > /dev/null

# 5 次测量
for i in {1..5}; do
  echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
  t=$(gsql -p "$PORT" -U omm -d postgres -c "$SQL" \
      | awk '/Total runtime:/ {print $3}')
  echo "${TABLE},$i,${t}" >> "$OUTFILE"
  echo "  Run #$i: ${t} ms"
done

echo "Results written to $OUTFILE"

# are authenticated, which PostgreSQL user names they can use, which
