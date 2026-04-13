# plot_analysis.py
# Usage: python3 plot_analysis.py
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import sys

# ---------- avalanche plot ----------
df = pd.read_csv("avalanche.csv")
dists = df['hamming'].values
mean = np.mean(dists)
std = np.std(dists, ddof=1)
print(f"Avalanche: samples={len(dists)}, mean={mean:.3f}, std={std:.3f}")

plt.figure(figsize=(8,5))
plt.hist(dists, bins=40, color='C0', alpha=0.8)
plt.title(f"Avalanche Hamming distances (mean={mean:.2f}, std={std:.2f})")
plt.xlabel("Hamming distance")
plt.ylabel("Count")
plt.grid(alpha=0.3)
plt.tight_layout()
plt.savefig("avalanche_hist.png")
print("Saved avalanche_hist.png")

# ---------- bias plot ----------
bias = pd.read_csv("bias.csv")
bits = bias['bit_index'].values
fractions = bias['sample_fraction'].values
mean_frac = np.mean(fractions)
print(f"Bias: mean fraction of ones across bits = {mean_frac:.4f}")

plt.figure(figsize=(12,4))
plt.bar(bits, fractions, width=1.0)
plt.axhline(0.5, color='red', linestyle='--', label='0.5')
plt.title("Per-bit 1-frequency (bias test)")
plt.xlabel("Bit index (0 is MSB of first byte)")
plt.ylabel("Fraction of ones")
plt.legend()
plt.tight_layout()
plt.savefig("bias_bar.png")
print("Saved bias_bar.png")

# ---------- collision summary ----------
col = pd.read_csv("collisions.csv")
collisions = int(col['collisions'][0])
total = int(col['total_samples'][0])
print(f"Collision summary: collisions={collisions}, samples={total}")
with open("collisions_summary.txt", "w") as f:
    f.write(f"collisions={collisions}, samples={total}\n")
print("Saved collisions_summary.txt")

print("Done. Created: avalanche_hist.png, bias_bar.png, collisions_summary.txt")
