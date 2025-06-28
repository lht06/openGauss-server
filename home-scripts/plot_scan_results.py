import pandas as pd
import matplotlib.pyplot as plt
import os

# 1. 读取合并后的扫描测试结果
csv_path = os.path.expanduser("~/scan_results.csv")
scan_df = pd.read_csv(csv_path)

# 2. 绘制折线图
plt.figure()
for table, grp in scan_df.groupby("table"):
    plt.plot(grp["run"], grp["ms"], marker='o', label=table)
plt.xlabel("Run 序号")
plt.ylabel("耗时 (ms)")
plt.title("单表扫描排序耗时对比")
plt.legend()

# 3. 保存图片
out_png = os.path.expanduser("~/scan_times.png")
plt.savefig(out_png, bbox_inches='tight')
print(f"已生成图片：{out_png}")

