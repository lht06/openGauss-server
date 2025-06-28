#!/usr/bin/env bash
set -euo pipefail

# 临时恢复 PATH，使系统命令可用
export PATH=/usr/local/bin:/usr/bin:/bin:$PATH
echo "[1/5] 临时恢复 PATH: \$PATH"

# 修复 ~/.bashrc 中 PATH 覆盖逻辑
BASHRC=~/.bashrc
echo "[2/5] 修复 \$BASHRC 中的 PATH 设置…"
if grep -q 'export PATH=.*\$GAUSSHOME/bin' "\$BASHRC"; then
  sed -i -E "s#export PATH=.*\\\$GAUSSHOME/bin.*#export PATH=\$GAUSSHOME/bin:/usr/local/bin:/usr/bin:/bin:\$PATH#g" "\$BASHRC"
else
  cat << 'EORS' >> "\$BASHRC"

# openGauss CLI & System PATH 保留
export PATH=\$GAUSSHOME/bin:/usr/local/bin:/usr/bin:/bin:\$PATH
EORS
fi

# 修正 setup 脚本的 shebang
SETUP=~/setup_opengauss_dual.sh
echo "[3/5] 修正脚本 \$SETUP 的 shebang…"
if [ -f "\$SETUP" ]; then
  sed -i '1s@.*@#!/usr/bin/env bash@' "\$SETUP"
  chmod +x "\$SETUP"
else
  echo "⚠️ 警告: 未找到 \$SETUP，跳过 shebang 修正"
fi

# 让新 rc 生效
echo "[4/5] source ~/.bashrc"
# shellcheck source=/dev/null
source "\$BASHRC"

# 用绝对 bash 路径运行 setup 脚本
echo "[5/5] 调用 setup 脚本…"
if [ -f "\$SETUP" ]; then
  /usr/bin/env bash "\$SETUP"
else
  echo "❌ 找不到 setup 脚本，无法执行"
  exit 1
fi

echo "✔️ 全部步骤执行完毕！"
