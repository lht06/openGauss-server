#!/usr/bin/env bash
# shrink_tables.sh —— 重建为 2M、10M、20M 行测试表

PORT=5432
USER=omm
DB=postgres

for NEW_SIZE in 2000000 10000000 20000000; do
  TBL="demo_${NEW_SIZE}"
  echo ">>> Rebuilding ${TBL} with ${NEW_SIZE} rows"
  gsql -p $PORT -U $USER -d $DB <<EOF
DROP TABLE IF EXISTS ${TBL};
-- 如果不需要 WAL，可改成 UNLOGGED 加速建表
-- CREATE UNLOGGED TABLE ${TBL} AS
CREATE TABLE ${TBL} AS
  SELECT * FROM generate_series(1,${NEW_SIZE}) AS val;
EOF
done

echo ">>> All tables rebuilt: demo_2000000, demo_10000000, demo_20000000"

