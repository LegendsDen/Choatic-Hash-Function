"""
plot_dtstcs.py  –  Generate plots for all DT-STCS test CSVs
Run from the directory containing the CSV files:
    pip install matplotlib pandas numpy seaborn
    python plot_dtstcs.py
"""

import os
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
from matplotlib.colors import LinearSegmentedColormap
import warnings
warnings.filterwarnings("ignore")

OUT_DIR = "plots"
os.makedirs(OUT_DIR, exist_ok=True)

PALETTE   = "#2563EB"      # primary blue
IDEAL_COL = "#DC2626"      # red for ideal lines
GRID_ARGS = dict(color="#E5E7EB", linewidth=0.6, linestyle="--")


def save(fig, name):
    path = os.path.join(OUT_DIR, name)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)


# ─────────────────────────────────────────────────────────────
# TEST 1  –  Hash Value Distribution
# ─────────────────────────────────────────────────────────────
def plot_test1():
    csv = "test1_distribution.csv"
    if not os.path.exists(csv):
        print(f"[skip] {csv} not found"); return

    df = pd.read_csv(csv)
    hex_labels = [hex(int(x))[2:].upper() for x in df["hex_digit"]]
    counts = df["count"].values

    # Mathematical Expected Value
    expected = counts.mean()
    # Chi-Squared Statistic: sum((observed - expected)^2 / expected)
    chi2 = ((counts - expected) ** 2 / expected).sum()

    fig, axes = plt.subplots(1, 2, figsize=(14, 6)) # Increased height slightly
    fig.suptitle("Test 1 – Hash Value Distribution", fontsize=14, fontweight="bold")

    # --- Plot 1: Nibble frequency histogram ---
    ax = axes[0]
    bars = ax.bar(hex_labels, counts, color=PALETTE, edgecolor="white", linewidth=0.4)
    ax.axhline(expected, color=IDEAL_COL, linewidth=1.5, linestyle="--",
               label=f"Expected = {expected:.1f}")

    ax.set_xlabel("Hex Digit")
    ax.set_ylabel("Frequency")
    ax.set_title("Nibble frequency histogram")

    # FIX 1: Give more headroom on Y-axis so text doesn't overlap top line
    ax.set_ylim(0, max(counts) * 1.25)

    ax.legend(loc="upper left", fontsize=9)
    ax.grid(axis="y", **GRID_ARGS)

    # FIX 2: Move annotation lower and use a background box for visibility
    # transform=ax.transAxes means (1,1) is the top right corner of the plot area
    ax.text(0.95, 0.92, f"$\chi^2$ = {chi2:.2f}\n(ideal < 24.996)",
            transform=ax.transAxes,
            ha="right", va="top", fontsize=10, fontweight='bold',
            bbox=dict(boxstyle="round,pad=0.5", fc="white", ec="black", alpha=0.9))

    # --- Plot 2: Deviation from uniform ---
    ax2 = axes[1]
    dev = counts - expected
    colors = [PALETTE if d >= 0 else "#EF4444" for d in dev]
    ax2.bar(hex_labels, dev, color=colors, edgecolor="white", linewidth=0.4)
    ax2.axhline(0, color="black", linewidth=0.8)
    ax2.set_xlabel("Hex Digit")
    ax2.set_ylabel("Count − Expected")
    ax2.set_title("Deviation from uniform distribution")
    ax2.grid(axis="y", **GRID_ARGS)

    # FIX 3: Tight layout with padding to prevent label clipping
    fig.tight_layout(rect=[0, 0.03, 1, 0.95])
    save(fig, "test1_distribution.png")


# ─────────────────────────────────────────────────────────────
# TEST 2  –  Sensitivity (Bit Flip)
# ─────────────────────────────────────────────────────────────
def plot_test2():
    csv = "test2_sensitivity.csv"
    if not os.path.exists(csv):
        print(f"[skip] {csv} not found"); return

    df = pd.read_csv(csv)
    cases   = df["case"].values
    bits    = df["changed_bits"].values
    pct     = df["percent"].values
    ideal   = 128.0   # 50% of 256 bits

    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle("Test 2 – Sensitivity / Avalanche Effect", fontsize=14, fontweight="bold")

    colors = [PALETTE if b > 0 else "#9CA3AF" for b in bits]

    ax = axes[0]
    short = [c.replace("message", "msg").replace("first", "1st").replace("last", "lst") for c in cases]
    bars = ax.barh(range(len(short)), bits, color=colors, edgecolor="white")
    ax.axvline(ideal, color=IDEAL_COL, linewidth=1.5, linestyle="--", label=f"Ideal = {ideal:.0f}")
    ax.set_yticks(range(len(short))); ax.set_yticklabels(short, fontsize=8)
    ax.set_xlabel("Changed bits"); ax.set_title("Bits changed vs. original hash")
    ax.legend(fontsize=8); ax.grid(axis="x", **GRID_ARGS)
    for i, v in enumerate(bits):
        ax.text(v + 0.5, i, str(v), va="center", fontsize=7)

    ax2 = axes[1]
    ax2.barh(range(len(short)), pct, color=colors, edgecolor="white")
    ax2.axvline(50.0, color=IDEAL_COL, linewidth=1.5, linestyle="--", label="Ideal = 50%")
    ax2.set_yticks(range(len(short))); ax2.set_yticklabels(short, fontsize=8)
    ax2.set_xlabel("% bits changed"); ax2.set_title("Percentage of bits changed")
    ax2.legend(fontsize=8); ax2.grid(axis="x", **GRID_ARGS)
    for i, v in enumerate(pct):
        ax2.text(v + 0.2, i, f"{v:.1f}%", va="center", fontsize=7)

    fig.tight_layout()
    save(fig, "test2_sensitivity.png")


# ─────────────────────────────────────────────────────────────
# TEST 3  –  Diffusion & Confusion
# ─────────────────────────────────────────────────────────────
def plot_test3():
    csv = "test3_diffusion.csv"
    if not os.path.exists(csv):
        print(f"[skip] {csv} not found"); return

    df = pd.read_csv(csv)
    bits = df["changed_bits"].values
    ideal = 128.0
    mean  = bits.mean()
    std   = bits.std()

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    fig.suptitle("Test 3 – Diffusion & Confusion", fontsize=14, fontweight="bold")

    # Histogram
    ax = axes[0]
    ax.hist(bits, bins=range(min(bits), max(bits) + 1), color=PALETTE, edgecolor="white", linewidth=0.4, alpha=0.85)
    ax.axvline(ideal, color=IDEAL_COL, linewidth=1.5, linestyle="--", label=f"Ideal = {ideal:.0f}")
    ax.axvline(mean,  color="#F59E0B",  linewidth=1.5, linestyle="-",  label=f"Mean = {mean:.2f}")
    ax.set_xlabel("Changed bits per trial"); ax.set_ylabel("Frequency")
    ax.set_title("Distribution of bit changes")
    ax.legend(fontsize=8); ax.grid(axis="y", **GRID_ARGS)

    # Trial-by-trial line
    ax2 = axes[1]
    ax2.plot(df["trial"], bits, color=PALETTE, linewidth=0.5, alpha=0.7)
    ax2.axhline(ideal, color=IDEAL_COL, linewidth=1.2, linestyle="--", label=f"Ideal = {ideal:.0f}")
    ax2.axhline(mean,  color="#F59E0B",  linewidth=1.2, linestyle="-",  label=f"Mean = {mean:.2f}")
    ax2.fill_between(df["trial"], mean - std, mean + std, alpha=0.15, color=PALETTE,
                     label=f"±1σ ({std:.2f})")
    ax2.set_xlabel("Trial index"); ax2.set_ylabel("Changed bits")
    ax2.set_title("Per-trial avalanche")
    ax2.legend(fontsize=8); ax2.grid(**GRID_ARGS)

    fig.tight_layout()
    save(fig, "test3_diffusion.png")


# ─────────────────────────────────────────────────────────────
# TEST 4  –  Collision Resistance WN(ω)
# ─────────────────────────────────────────────────────────────
def plot_test4():
    csv = "test4_collision.csv"
    if not os.path.exists(csv):
        print(f"[skip] {csv} not found"); return

    df = pd.read_csv(csv)
    # Ensure w is numeric for proper spacing
    w = df["w"].values
    exp = df["experimental"].values
    theo = df["theoretical"].values

    fig, ax = plt.subplots(figsize=(12, 6))
    fig.suptitle("Test 4 – Collision Resistance $W_N(\omega)$", fontsize=14, fontweight="bold")

    # Use width based on the numeric scale
    width = 0.4

    ax.bar(w - width/2, exp, width, label="Experimental", color=PALETTE, alpha=0.9)
    # Plotting theoretical as a line or steps often looks better for distributions
    ax.bar(w + width/2, theo, width, label="Theoretical", color="#10B981", alpha=0.6)

    # FIX: Show ticks every 5 or 10 units instead of every single integer
    ax.xaxis.set_major_locator(mticker.MultipleLocator(5))

    ax.set_xlabel("$\omega$ (Number of matching bits)")
    ax.set_ylabel("Count")
    ax.set_title("Distribution of matching bits between hashed pairs ($N=2000$ trials)")

    ax.legend()
    ax.grid(axis="y", **GRID_ARGS)

    fig.tight_layout()
    save(fig, "test4_collision.png")


# ─────────────────────────────────────────────────────────────
# TEST 5  –  Absolute Difference
# ─────────────────────────────────────────────────────────────
def plot_test5():
    csv = "test5_absdiff.csv"
    if not os.path.exists(csv):
        print(f"[skip] {csv} not found"); return

    df = pd.read_csv(csv)
    data = dict(zip(df["metric"], df["value"]))

    labels   = ["Min", "Mean", "Theoretical\nIdeal", "Max"]
    values   = [data.get("min", 0), data.get("mean", 0),
                data.get("theoretical", 1360), data.get("max", 0)]
    colors   = ["#6EE7B7", PALETTE, IDEAL_COL, "#FCA5A5"]

    fig, ax = plt.subplots(figsize=(8, 5))
    fig.suptitle("Test 5 – Absolute Difference", fontsize=14, fontweight="bold")

    bars = ax.bar(labels, values, color=colors, edgecolor="white", width=0.5)
    ax.axhline(data.get("theoretical", 1360), color=IDEAL_COL,
               linewidth=1.4, linestyle="--", label=f"Theoretical ≈ {data.get('theoretical',1360):.0f}")
    ax.set_ylabel("Mean absolute difference"); ax.set_title("Absolute difference statistics")
    ax.legend(fontsize=9); ax.grid(axis="y", **GRID_ARGS)
    for bar, val in zip(bars, values):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 5,
                f"{val:.1f}", ha="center", va="bottom", fontsize=9)

    fig.tight_layout()
    save(fig, "test5_absdiff.png")


# ─────────────────────────────────────────────────────────────
# TEST 6  –  SAC Dependence Matrix Heatmap
# ─────────────────────────────────────────────────────────────
def plot_test6():
    csv = "test6_sac.csv"
    if not os.path.exists(csv):
        print(f"[skip] {csv} not found"); return

    df = pd.read_csv(csv)
    n   = df["output_bit"].max() + 1
    m   = df["input_bit"].max() + 1
    mat = np.zeros((n, m))
    for _, row in df.iterrows():
        mat[int(row["output_bit"]), int(row["input_bit"])] = row["probability"]

    cmap = LinearSegmentedColormap.from_list("sac",
           ["#1E3A5F", "#2563EB", "#93C5FD", "#FEF3C7", "#F59E0B", "#DC2626"])

    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle("Test 6 – Strict Avalanche Criterion (SAC)", fontsize=14, fontweight="bold")

    im = axes[0].imshow(mat, aspect="auto", cmap=cmap, vmin=0, vmax=1,
                        interpolation="nearest")
    axes[0].set_xlabel("Input bit flipped"); axes[0].set_ylabel("Output bit")
    axes[0].set_title("SAC dependence matrix\n(ideal = 0.5 everywhere)")
    fig.colorbar(im, ax=axes[0], label="P(bit changes)")

    # Histogram of all probabilities
    ax2 = axes[1]
    flat = mat.flatten()
    ax2.hist(flat, bins=40, color=PALETTE, edgecolor="white", linewidth=0.3, alpha=0.85)
    ax2.axvline(0.5, color=IDEAL_COL, linewidth=1.5, linestyle="--", label="Ideal = 0.5")
    ax2.axvline(flat.mean(), color="#F59E0B", linewidth=1.5, linestyle="-",
                label=f"Mean = {flat.mean():.4f}")
    ax2.set_xlabel("Probability"); ax2.set_ylabel("Frequency")
    ax2.set_title("Distribution of SAC probabilities")
    ax2.legend(fontsize=8); ax2.grid(axis="y", **GRID_ARGS)

    fig.tight_layout()
    save(fig, "test6_sac.png")


# ─────────────────────────────────────────────────────────────
# TEST 7  –  Performance Benchmark
# ─────────────────────────────────────────────────────────────
def plot_test7():
    csv = "test7_performance.csv"
    if not os.path.exists(csv):
        print(f"[skip] {csv} not found"); return

    df = pd.read_csv(csv)
    labels = [str(x) for x in df["msg_bytes"]]

    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    fig.suptitle("Test 7 – Performance Benchmark", fontsize=14, fontweight="bold")

    ax = axes[0]
    bars = ax.bar(labels, df["throughput_mbps"], color=PALETTE, edgecolor="white")
    ax.set_xlabel("Message size (bytes)"); ax.set_ylabel("Throughput (MB/s)")
    ax.set_title("Throughput by message size")
    ax.grid(axis="y", **GRID_ARGS)
    for bar, val in zip(bars, df["throughput_mbps"]):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.01,
                f"{val:.2f}", ha="center", va="bottom", fontsize=8)

    ax2 = axes[1]
    ax2.bar(labels, df["time_ms"], color="#7C3AED", edgecolor="white")
    ax2.set_xlabel("Message size (bytes)"); ax2.set_ylabel("Total time (ms)")
    ax2.set_title("Total benchmark time by message size")
    ax2.grid(axis="y", **GRID_ARGS)

    fig.tight_layout()
    save(fig, "test7_performance.png")


# ─────────────────────────────────────────────────────────────
# TEST 8  –  Cross-Thread Coupling
# ─────────────────────────────────────────────────────────────
def plot_test8():
    csv = "test8_coupling.csv"
    if not os.path.exists(csv):
        print(f"[skip] {csv} not found"); return

    df = pd.read_csv(csv)
    hamming = int(df[df["key"] == "modified"]["hamming_to_default"].values[0])
    total   = 256

    fig, ax = plt.subplots(figsize=(7, 5))
    fig.suptitle("Test 8 – Cross-Thread Coupling Verification", fontsize=14, fontweight="bold")

    labels  = ["Changed bits", "Unchanged bits"]
    sizes   = [hamming, total - hamming]
    colors  = [PALETTE, "#E5E7EB"]
    explode = (0.05, 0)
    wedges, texts, autotexts = ax.pie(
        sizes, labels=labels, colors=colors, explode=explode,
        autopct="%1.1f%%", startangle=90,
        wedgeprops=dict(edgecolor="white", linewidth=1.5))
    for at in autotexts: at.set_fontsize(11)
    ax.set_title(f"Hamming distance = {hamming} / {total} bits\nafter tiny key perturbation (1e-10)")

    fig.tight_layout()
    save(fig, "test8_coupling.png")


# ─────────────────────────────────────────────────────────────
# MAIN
# ─────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print("DT-STCS Plot Generator")
    print(f"  Output directory: {os.path.abspath(OUT_DIR)}\n")

    plot_test1()
    plot_test2()
    plot_test3()
    plot_test4()
    plot_test5()
    plot_test6()
    plot_test7()
    plot_test8()

    print("\nDone. All plots saved to ./plots/")
