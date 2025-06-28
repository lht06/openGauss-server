#!/usr/bin/env python3
import pandas as pd
import matplotlib.pyplot as plt
import os

# 1. 读取 CSV
csv_path = os.path.expanduser("~/pgbench_results.csv")
bench_df = pd.read_csv(csv_path)

# 2. 绘制折线图
plt.figure()
plt.plot(bench_df["clients"], bench_df["tps"], marker='o')
plt.xlabel("并发客户端数")
plt.ylabel("TPS")
plt.title("pgbench 并发吞吐测试")
plt.xticks(bench_df["clients"])

# 3. 保存为 PNG
out_png = os.path.expanduser("~/pgbench_tps.png")
plt.savefig(out_png, bbox_inches='tight')
print(f"已生成图片：{out_png}")

