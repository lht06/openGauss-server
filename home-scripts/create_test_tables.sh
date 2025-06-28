#!/usr/bin/env bash
for SIZE in 2000000 10000000 20000000; do
  echo "==> 创建 demo_${SIZE}"
  gsql -d postgres -p 5432 -U omm << SQL
DROP TABLE IF EXISTS demo_${SIZE};
CREATE TABLE demo_${SIZE} AS SELECT generate_series(1,${SIZE}) AS val;
SQL
done
