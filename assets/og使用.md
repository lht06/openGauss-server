下面是“傻瓜式”一览表，把所有关键命令、路径、端口和注意事项都罗列出来，按步照做就不会出错。

------

## 一、环境变量（登录后自动生效）

```bash
# 写在 ~/.bash_profile 或 ~/.bashrc 中，登录时自动加载
export CODE_BASE=/home/omm/openGauss-base      # 如要切到修改版，改为 openGauss-mod
export GAUSSHOME=$CODE_BASE/mppdb_temp_install
export LD_LIBRARY_PATH=$GAUSSHOME/lib:$LD_LIBRARY_PATH
export PATH=$GAUSSHOME/bin:/usr/local/bin:/usr/bin:/bin:$PATH
```

**只要登录 shell，这几行就生效，后面所有命令都可以直接用。**

------

## 二、目录 & 端口

| 实例 | 源码目录                 | 数据目录            | 日志目录           | 端口 |
| ---- | ------------------------ | ------------------- | ------------------ | ---- |
| 基线 | /home/omm/openGauss-base | /home/omm/data/base | /home/omm/log/base | 5432 |
| 修改 | /home/omm/openGauss-mod  | /home/omm/data/mod  | /home/omm/log/mod  | 5433 |

------

## 三、启动 & 停止

- **启动基线实例**

  ```bash
  gs_ctl start \
    -D ~/data/base \
    -Z single_node \
    -l ~/log/base/openGauss.log
  ```

- **启动修改实例**
   如果你已在 `data/mod/postgresql.conf` 指定 `port = 5433`，直接：

  ```bash
  gs_ctl start \
    -D ~/data/mod \
    -Z single_node \
    -l ~/log/mod/openGauss.log
  ```

  否则加 `-o "-p 5433"`：

  ```bash
  gs_ctl start \
    -D ~/data/mod \
    -Z single_node \
    -o "-p 5433" \
    -l ~/log/mod/openGauss.log
  ```

- **停止实例**

  ```bash
  gs_ctl stop -D ~/data/base
  gs_ctl stop -D ~/data/mod
  ```

------

## 四、连接 & 登录

1. **基线实例**

   ```bash
   gsql -d postgres -p 5432 -U omm
   # 看到 “Password for user omm:” 再输入密码即可
   ```

2. **修改实例**

   ```bash
   gsql -d postgres -p 5433 -U omm
   ```

3. **非交互式**（在命令行里指定密码，不建议常用，安全性较差）

   ```bash
   gsql -d postgres -p 5432 -U omm -W YourPassword
   ```

4. **环境变量方式**

   ```bash
   export PGPASSWORD='YourPassword'
   gsql -d postgres -p 5432 -U omm
   unset PGPASSWORD
   ```

------

## 五、常用 SQL 测试模板

```sql
-- 登录后复制粘贴执行：

-- 查看服务器版本
SELECT version();

-- 创建测试表
DROP TABLE IF EXISTS demo;
CREATE TABLE demo(id SERIAL PRIMARY KEY, info TEXT);

-- 插入两行数据
INSERT INTO demo(info) VALUES ('foo'), ('bar');

-- 查询
SELECT * FROM demo;

-- 删除测试表
DROP TABLE demo;

-- 退出
\q
```

------

## 六、性能对比（可选）

1. 安装并初始化 pgbench（由 vagrant 或 root 执行）：

   ```bash
   sudo yum install -y postgresql-contrib
   pgbench -i -s 10 -p 5432 postgres  # 基线
   pgbench -i -s 10 -p 5433 postgres  # 修改
   ```

2. 运行 60 秒测试：

   ```bash
   pgbench -T 60 -c 10 -j 2 -p 5432 postgres > base.txt
   pgbench -T 60 -c 10 -j 2 -p 5433 postgres > mod.txt
   ```

3. 对比 TPS：

   ```bash
   grep tps base.txt
   grep tps mod.txt
   ```

------

## 七、遇到问题时

- **命令找不到**：先确认 `echo $PATH` 包含 `/home/omm/openGauss-*/mppdb_temp_install/bin`
- **端口占用**：`netstat -tunlp | grep 5432` / `5433`，如有旧进程先 `kill`
- **日志查看**：`tail -n 50 ~/log/<base|mod>/openGauss.log`，或 `grep -iE "ERROR|FATAL"`
- **重置数据目录**：如需重新 `initdb`，先 `rm -rf ~/data/mod/*` 再执行 `gs_initdb`

------

照着这张“傻瓜式”清单做，就能无障碍管理、连接、测试两个 OpenGauss 实例。加油！