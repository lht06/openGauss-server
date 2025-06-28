#!/bin/bash
set -euo pipefail

# 脚本假设以 omm 身份运行，或通过 sudo su - omm 切换到 omm
WORKDIR=/home/omm
THIRD_PARTY_TGZ=openGauss-third_party_binarylibs_Centos7.6_x86_64.tar.gz
THIRD_PARTY_URL="https://opengauss.obs.cn-south-1.myhuaweicloud.com/latest/binarylibs/${THIRD_PARTY_TGZ}"
BINARYLIBS_DIR=binarylibs
REPO_URL="https://gitee.com/opengauss/openGauss-server.git"
BUILD_MODE=debug
MAKE_JOBS=4

cd "$WORKDIR"

echo "==> 1. 准备 third-party 依赖"
if [ ! -d "$BINARYLIBS_DIR" ]; then
  wget "$THIRD_PARTY_URL"
  tar zxvf "$THIRD_PARTY_TGZ"
  mv openGauss-third_party_binarylibs_Centos7.6_x86_64 "$BINARYLIBS_DIR"
  rm -f "$THIRD_PARTY_TGZ"
fi

for NAME in base mod; do
  SRC_DIR="openGauss-$NAME"
  echo
  echo "==> 2. 克隆源码到 $SRC_DIR"
  if [ ! -d "$SRC_DIR" ]; then
    git clone "$REPO_URL" "$SRC_DIR"
  fi

  echo "==> 3. 修改并发编译参数（$SRC_DIR）"
  CMAKE_SCRIPT="$WORKDIR/$SRC_DIR/build/script/utils/cmake_compile.sh"
  # 将 make -sj* 替换为 make -j4
  sed -i -E "s/make\s+-s*j[0-9]*/make -j$MAKE_JOBS/g" "$CMAKE_SCRIPT"

  echo "==> 4. 编译源码（$SRC_DIR）"
  cd "$WORKDIR/$SRC_DIR"
  sh build.sh -m "$BUILD_MODE" -3rd "$WORKDIR/$BINARYLIBS_DIR" \
    2>&1 | tee build/script/makemppdb_pkg.log

  echo "==> 5. 安装完成，切回主目录"
  cd "$WORKDIR"
done

echo
echo "==> 6. 配置环境变量（追加到 ~/.bashrc）"
cat << 'EOF' >> ~/.bashrc

# openGauss 环境
export CODE_BASE=/home/omm/openGauss-base      # 或者 openGauss-mod，根据需要切换
export GAUSSHOME=\$CODE_BASE/mppdb_temp_install
export LD_LIBRARY_PATH=\$GAUSSHOME/lib::\$LD_LIBRARY_PATH
export PATH=\$GAUSSHOME/bin:\$PATH
EOF

echo "请执行： source ~/.bashrc"

echo
echo "==> 7. 初始化数据库目录 & 启动（示例）"
# 你也可以分别用不同 data 目录来做对比测试
mkdir -p ~/data/base ~/log/base
mkdir -p ~/data/mod  ~/log/mod

# 初始化并启动 base 实例
gs_initdb -D /home/omm/data/base --nodename=gauss_base
gs_ctl start -D /home/omm/data/base -Z single_node -l /home/omm/log/base/openGauss.log

# 初始化并启动 mod 实例（用不同端口）
gs_initdb -D /home/omm/data/mod  --nodename=gauss_mod
gs_ctl start -D /home/omm/data/mod  -Z single_node -p 5433 -l /home/omm/log/mod/openGauss.log

echo
echo "==> 8. 防火墙开放端口 5432 & 5433"
sudo firewall-cmd --zone=public --add-port=5432/tcp --permanent
sudo firewall-cmd --zone=public --add-port=5433/tcp --permanent
sudo firewall-cmd --reload

echo
echo "全部完成！"
echo "– 基线实例: 5432 端口, 数据目录 ~/data/base"
echo "– 修改实例: 5433 端口, 数据目录 ~/data/mod"

