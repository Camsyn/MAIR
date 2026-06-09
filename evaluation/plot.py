#!/usr/bin/env /usr/bin/python3
'''
This script visualizes benchmark results. It reads benchmark data,
computes overheads for different Sanitizer combinations, and generates plots for comparing performance impact.

Generated plots include:
    1. Overhead plots for individual sanitizer modes ({bar, violin} x {CPU, RSS}).
    2. Comparison plots for Sanitizer combinations and corresponding XSan combinations ({bar, violin} x {CPU, RSS}).
    3. Per-program overhead plots ({bar} x {regular, compare} x {CPU, RSS}).

Usage:
    python3 ./plot.py [-f]

Options:
    -f              Force regeneration of data and overwrite existing results

Workflow:
    1. Load benchmark configuration and result data
    2. Compute overheads for each Sanitizer and combination
    3. Draw and save comparison figures:
         - Per-program plots saved under <package>/data/<program>
         - Overall plots saved in the evaluation/ directory

Adjust BENCH_TO_PLOT to select which benchmarks to visualize.
'''

import sys
from itertools import combinations, chain
from benchmarking import Bench, SANITIZERS
from typing import Iterable
from matplotlib import pyplot as plt
from matplotlib.patches import Patch
import pandas as pd
import numpy as np
import gen_bench_config
gen_bench_config.generate_bench_configs(lazy=True)
from benchmarks import PACKAGE_2_PROGRAMS, find_bench_config

BENCH_TO_PLOT = [
    *PACKAGE_2_PROGRAMS['7zip'],
    *PACKAGE_2_PROGRAMS['pcre2'],
    *PACKAGE_2_PROGRAMS['binutils'],
    *PACKAGE_2_PROGRAMS['zstd'],
    *PACKAGE_2_PROGRAMS['xz'],
    *PACKAGE_2_PROGRAMS['tpm2-tss'],
    *PACKAGE_2_PROGRAMS['strongswan'],
    *PACKAGE_2_PROGRAMS['lua'],
    *PACKAGE_2_PROGRAMS['iproute2'],
    *PACKAGE_2_PROGRAMS['cJSON'],
    *PACKAGE_2_PROGRAMS['json'],
    *PACKAGE_2_PROGRAMS['jsoncpp'],
    *PACKAGE_2_PROGRAMS['zlib'],
    *PACKAGE_2_PROGRAMS['libjpeg-turbo'],
    *PACKAGE_2_PROGRAMS['sqlite3'],
    *PACKAGE_2_PROGRAMS['libpng'],
    *PACKAGE_2_PROGRAMS['re2'],
    *PACKAGE_2_PROGRAMS['curl'],
    *PACKAGE_2_PROGRAMS['lcms'],
    *PACKAGE_2_PROGRAMS['libxml2'],
    *PACKAGE_2_PROGRAMS['openssl'],
]

FORCE = False
OUTPUT_FORMAT = 'png'
DPI = 600

def get_overheads(data: np.ndarray) -> np.ndarray:
    '''
    get the overheads of the data

    Input: M * N * K, M = len(sanitizers), N = len(corpus), K = REPEAT_TIMES
       data[0] is the base
    '''
    return (data[1:] / data[0]) * 100


def sumup_data(overheads: np.ndarray, san_comb: Iterable, sanitizers: list) -> (np.ndarray, str):
    '''
    extend the data to include the new 'sanitizer', e.g.,, ASan + TSan + MSan + UBSan
    1. Find the indices of the sanitizers in the sanitizers list
    2. Compute the overhead of the new 'sanitizer' as the sum of the overheads of the sanitizers in the sanitizer combination
    3. Concatenate the new 'sanitizer' data
    '''
    indices = [sanitizers.index(san) for san in san_comb]
    if overheads.ndim == 2:
        # Handle N * M case
        new_overhead = np.sum(overheads[:, indices], axis=1)
    elif overheads.ndim == 1:
        # Handle M case
        new_overhead = np.sum(overheads[indices])
    else:
        raise ValueError(
            "Overheads dimension not supported: {}".format(overheads.shape))
    return new_overhead, '+'.join(san_comb)


def get_xsan_data(overheads: np.ndarray, san_comb: Iterable, sanitizers: list) -> (np.ndarray, str):
    '''
    get the data of the xsan
    '''
    name = 'xsan-' + '-'.join(san_comb)
    index = sanitizers.index(name)
    if overheads.ndim == 2:
        overhead = overheads[:, index]
    elif overheads.ndim == 1:
        overhead = overheads[index]
    else:
        raise ValueError(
            "Overheads dimension not supported: {}".format(overheads.shape))
    return overhead, name


def plot_figure(is_time: bool):
    '''
    Plot the figure of the overheads of the sanitizers on the benchmarks
    Args:
        is_time: bool, if True, plot the figure of the time overheads of the sanitizers on the benchmarks
            if False, plot the figure of the RSS overheads of the sanitizers on the benchmarks
    '''
    overheads = []
    sanitizers = SANITIZERS.copy()
    sanitizers.remove('raw')
    group = "time" if is_time else "rss"
    for bench in BENCH_TO_PLOT:
        config = find_bench_config(bench)
        bench = Bench(config)
        '''
        ignore_init=True: subtract fixed initialization cost so very short programs can be measured
        '''
        data = bench.load_overhead_batchly(sanitizers, group, ignore_init=False, depth=2)
        draw_per_bench_figures(data, sanitizers, bench, is_time)
        overheads.append(data)

    overheads = np.array(overheads)
    draw_overall_figures(overheads, sanitizers,
                         ignore_small=False, is_rss=not is_time)
    # Draw Sum vs XSan comparison charts
    draw_overall_compare_figure(overheads, sanitizers, is_time)


def draw_per_bench_figures(overheads: np.ndarray, sanitizers: list, bench: Bench, is_time: bool):
    '''
    Draw the bar chart of the overhead of the sanitizers on the benchmark
    Args:
        overhead: np.ndarray, the overhead of the sanitizers on the benchmark
        sanitizers: list, the sanitizers
        bench: Bench, the benchmark
    '''
    name = 'time' if is_time else 'rss'
    bar_fig_path = bench.get_data_file(f'overhead_{name}.{OUTPUT_FORMAT}')
    cmp_fig_path = bench.get_data_file(f'compare_{name}.{OUTPUT_FORMAT}')

    def draw_bar():
        M = len(sanitizers)
        x_indices = np.arange(M)
        bar_width = 0.8

        # Create a figure with three rows: title / legend / main plot
        fig = plt.figure(figsize=(12, 6))
        gs = fig.add_gridspec(3, 1, height_ratios=[
                              0.08, 0.14, 1.0], hspace=0.05)

        # Title row
        ax_title = fig.add_subplot(gs[0])
        ax_title.axis("off")
        title_text = '{} Overheads of Sanitizers on ${}$'.format(
            "CPU" if is_time else "Memory", bench.program)
        ax_title.text(0.5, 0.5, title_text,
                      ha="center", va="center",
                      fontsize=14, fontweight="bold")

        # Legend row
        ax_legend = fig.add_subplot(gs[1])
        ax_legend.axis("off")

        # Main plot
        ax = fig.add_subplot(gs[2])

        linewidth = 0.4

        # Generate different colors for each bar
        cmap = plt.get_cmap("tab20")
        colors = [cmap(i % 20) for i in range(M)]

        # Draw each bar with its own color
        bars = ax.bar(
            x_indices,
            overheads,
            width=bar_width,
            color=colors,
            label=sanitizers,
            alpha=0.9,
            edgecolor="black",
            linewidth=linewidth
        )

        # Label each bar with its numeric value
        for rect, value in zip(bars, overheads):
            height = rect.get_height()
            ax.text(
                rect.get_x() + rect.get_width() / 2,
                height + 1,
                f'{value:.2f}',
                ha='center',
                va='bottom',
                fontsize=9
            )

        # Configure x-axis
        ax.set_xticks(x_indices)
        ax.set_xticklabels([])

        ax.set_xlabel("Benchmarks")
        ax.set_ylabel("Overhead (%)")

        # baseline
        ax.axhline(y=100, color="red", linestyle="--", linewidth=1)

        # Draw legend in the dedicated legend row
        ncol = min(6, M)  # max 6 columns in the legend
        ax_legend.legend(
            handles=bars,
            labels=sanitizers,
            loc="center",
            ncol=ncol,
            frameon=False,
            fontsize=9
        )

        # Save figure
        plt.savefig(bar_fig_path, dpi=DPI, bbox_inches='tight')
        plt.close(fig)

    def draw_compare_figure():
        # --- prepare sanitizer combinations (asan + powerset of other san) ---
        san_comb = list(chain.from_iterable(
            combinations(('msan', 'tsan', 'ubsan'), r)
            for r in range(0, len(sanitizers) + 1)
        ))
        san_comb = [('asan', *comb) for comb in san_comb]

        data_pair_to_compare = []
        san_pair_to_compare = []
        for comb in san_comb:
            sum_overhead, new_san = sumup_data(overheads, comb, sanitizers)
            xsan_overhead, xsan_name = get_xsan_data(
                overheads, comb, sanitizers)
            data_pair_to_compare.append(
                (np.asarray(sum_overhead), np.asarray(xsan_overhead)))
            san_pair_to_compare.append((new_san, xsan_name))

        # shared values
        M = len(data_pair_to_compare)

        x_indices = np.arange(M)
        group_width = 0.9
        pair_width = group_width
        bar_width = pair_width * 0.5

        fig = plt.figure(figsize=(8, 6))
        gs = fig.add_gridspec(3, 1, height_ratios=[
                              0.06, 0.16, 1.0], hspace=0.02)

        # title
        ax_title = fig.add_subplot(gs[0])
        ax_title.axis('off')
        title_text = f'{"CPU" if is_time else "Memory"} Overhead Comparison on ${bench.program}$: Sum vs XSan'
        ax_title.text(0.5, 0.5, title_text, ha='center',
                      va='center', fontsize=16, fontweight='bold')

        # legend row
        ax_legend = fig.add_subplot(gs[1])
        ax_legend.axis('off')

        # main axis
        ax = fig.add_subplot(gs[2])

        cmap = plt.get_cmap('tab20')
        colors = [cmap(i % 20) for i in range(M)]
        legend_handles = []
        linewidth = 0.4

        # Store bar containers for later annotation
        sum_bars = []
        xsan_bars = []

        for i, ((sum_data, xsan_data), (sum_name, xsan_name)) in enumerate(
            zip(data_pair_to_compare, san_pair_to_compare)
        ):
            # sum bar
            bar1 = ax.bar(
                x_indices[i] - bar_width / 2,
                sum_data,
                width=bar_width,
                color=colors[i],
                edgecolor='black',
                linewidth=linewidth,
                zorder=3,
                alpha=0.8  # transparency
            )
            sum_bars.append(bar1)

            # xsan bar
            bar2 = ax.bar(
                x_indices[i] + bar_width / 2,
                xsan_data,
                width=bar_width,
                color=colors[i],
                edgecolor='black',
                linewidth=linewidth,
                hatch='///',
                zorder=3,
                alpha=0.8  # transparency
            )
            xsan_bars.append(bar2)

            legend_handles.append(Patch(facecolor=colors[i],
                                        edgecolor='black',
                                        linewidth=linewidth,
                                        label=f'{sum_name} (Sum)'))
            legend_handles.append(Patch(facecolor=colors[i],
                                        edgecolor='black',
                                        hatch='///',
                                        linewidth=linewidth,
                                        label=f'{sum_name} (XSan)'))

        # Annotate bars with their numeric values
        for i in range(M):
            # Merge the sum and xsan bars
            for rect in zip(sum_bars[i], xsan_bars[i]):
                # (Sum-Bar, XSan-Bar)
                for r in rect:
                    height = r.get_height()
                    ax.text(
                        r.get_x() + r.get_width() / 2,
                        height + 1,  # make text above the top of the bar slightly
                        f'{height:.2f}',
                        ha='center',
                        va='bottom',
                        fontsize=8
                    )

        # ticks & labels
        ax.set_xticks(x_indices)
        ax.set_xticklabels([])
        ax.margins(x=0.01)

        # baseline
        ax.axhline(y=100, color='red', linestyle='--', linewidth=1, zorder=1)

        ax.set_ylabel("Overhead (%)")

        # legend
        if legend_handles:
            ncol = min(4, max(1, len(legend_handles)))
            ax_legend.legend(handles=legend_handles,
                             loc='center',
                             ncol=ncol,
                             frameon=False,
                             handlelength=1.2,
                             fontsize=9)

        fig.subplots_adjust(top=0.93, left=0.06, right=0.94, bottom=0.08)

        plt.savefig(cmp_fig_path, dpi=DPI, bbox_inches='tight')
        plt.close(fig)

    if FORCE or not bar_fig_path.exists():
        draw_bar()
    if FORCE or not cmp_fig_path.exists():
        draw_compare_figure()


def draw_overall_compare_figure(overheads: np.ndarray, sanitizers: list, is_time: bool):
    """
    Draw comparison figures (bar + violin) for Sum vs XSan overheads.
    Saves two files:
      - compare_sum_vs_xsan_{time|rss}.png   (bar chart)
      - compare_sum_vs_xsan_violin_{time|rss}.png  (violin plot)
    """
    # --- prepare sanitizer combinations (asan + powerset of other san) ---
    san_comb = list(chain.from_iterable(
        combinations(('msan', 'tsan', 'ubsan'), r)
        for r in range(0, len(sanitizers) + 1)
    ))
    san_comb = [('asan', *comb) for comb in san_comb]

    data_pair_to_compare = []
    san_pair_to_compare = []
    for comb in san_comb:
        sum_overhead, new_san = sumup_data(overheads, comb, sanitizers)
        xsan_overhead, xsan_name = get_xsan_data(overheads, comb, sanitizers)
        data_pair_to_compare.append(
            (np.asarray(sum_overhead), np.asarray(xsan_overhead)))
        san_pair_to_compare.append((new_san, xsan_name))

    mlen = max(len(bench) for bench in BENCH_TO_PLOT)
    for (new_san, xsan_name), (sum_overhead, xsan_overhead) in zip(san_pair_to_compare, data_pair_to_compare):
        speedups = 1 - (xsan_overhead / sum_overhead)
        print(f'{new_san}')
        for bench, speedup, xsan_oh, sum_oh in zip(BENCH_TO_PLOT, speedups, xsan_overhead, sum_overhead):
            print(f'\t{bench:{mlen}s} : {speedup * 100:8.2f}%, ( {xsan_oh:>8.2f} / {sum_oh:<8.2f} )')

        # shared values
    N = len(BENCH_TO_PLOT)
    M = len(data_pair_to_compare)

    def draw_bar():
        group_width = 0.8
        pair_width = group_width / max(M, 1)
        bar_width = pair_width * 0.45

        # Layout subplots: at most 5 columns per row
        ncols = 5
        nrows = int(np.ceil(N / ncols))

        # Scale overall figure size with row/column count
        fig_width = 3 * ncols
        fig_height = 2.5 * nrows + 2  # +2 for the title and legend
        fig = plt.figure(figsize=(fig_width, fig_height))
        gs = fig.add_gridspec(nrows + 2, ncols,
                              height_ratios=[0.1, 0.3] + [1.0] * nrows,
                              hspace=0.35, wspace=0.25)

        # --- title ---
        ax_title = fig.add_subplot(gs[0, :])
        ax_title.axis("off")
        title_text = f'{"CPU" if is_time else "Memory"} Overhead Comparison: Sum vs XSan'
        ax_title.text(0.5, 0.5, title_text, ha="center",
                      va="center", fontsize=16, fontweight="bold")

        # --- legend row ---
        ax_legend = fig.add_subplot(gs[1, :])
        ax_legend.axis("off")

        cmap = plt.get_cmap("tab20")
        colors = [cmap(i % 20) for i in range(M)]
        linewidth = 0.4

        # --- Draw each benchmark in its own subplot ---
        for bi, bench in enumerate(BENCH_TO_PLOT):
            row = bi // ncols + 2  # +2 for the title and legend
            col = bi % ncols
            ax = fig.add_subplot(gs[row, col])

            for i, ((sum_data, xsan_data), (sum_name, xsan_name)) in enumerate(
                zip(data_pair_to_compare, san_pair_to_compare)
            ):
                offset = (i - (M - 1) / 2) * pair_width + pair_width / 2

                # sum bar
                ax.bar(
                    [offset - bar_width / 2],
                    [sum_data[bi]],
                    width=bar_width,
                    color=colors[i],
                    edgecolor="black",
                    linewidth=linewidth,
                    zorder=3,
                    alpha=0.8
                )

                # xsan bar
                ax.bar(
                    [offset + bar_width / 2],
                    [xsan_data[bi]],
                    width=bar_width,
                    color=colors[i],
                    edgecolor="black",
                    linewidth=linewidth,
                    hatch="///",
                    zorder=3,
                    alpha=0.8
                )

            # baseline
            ax.axhline(y=100, color="red", linestyle="--",
                       linewidth=1, zorder=1)

            # Axis & title
            ax.set_title(bench, fontsize=10)
            ax.set_xticks([])  # no x-axis ticks
            ax.set_xlabel("")
            if col == 0:
                ax.set_ylabel("Overhead (%)")
            else:
                # Set ylabel for the all columns
                pass
                # ax.set_yticklabels([])

        # --- legend ---
        legend_handles = []
        for i, (sum_name, _) in enumerate(san_pair_to_compare):
            legend_handles.append(Patch(facecolor=colors[i],
                                        edgecolor='black',
                                        linewidth=linewidth,
                                        label=f'{sum_name} (Sum)'))
            legend_handles.append(Patch(facecolor=colors[i],
                                        edgecolor='black',
                                        hatch='///',
                                        linewidth=linewidth,
                                        label=f'{sum_name} (XSan)'))
        if legend_handles:
            ncol = min(4, max(1, len(legend_handles)))
            ax_legend.legend(handles=legend_handles,
                             loc="center",
                             ncol=ncol,
                             frameon=False,
                             handlelength=1.2,
                             fontsize=9)

        typ = "time" if is_time else "rss"
        plt.savefig(f"compare_{typ}.{OUTPUT_FORMAT}",
                    dpi=DPI, bbox_inches="tight")
        plt.close(fig)

    def draw_violin():
        # --- 1. Prepare data ---
        rows = []
        groups = []   # Track mapping from group name to color
        for idx, ((sum_data, xsan_data), (sum_name, xsan_name)) in enumerate(
            zip(data_pair_to_compare, san_pair_to_compare)
        ):
            group_id = f"{sum_name}"
            groups.append(group_id)
            for v in sum_data:
                rows.append({"Group": group_id, "Type": "Sum",
                            "Overhead": v, "X": idx})
            for v in xsan_data:
                rows.append({"Group": group_id, "Type": "XSan",
                            "Overhead": v, "X": idx})
        df = pd.DataFrame(rows)

        # --- 2. Color mapping ---
        cmap = plt.get_cmap("tab10")
        group_colors = {g: cmap(i % 10) for i, g in enumerate(groups)}

        fig = plt.figure(figsize=(10, 10))
        gs = fig.add_gridspec(3, 1, height_ratios=[
                              0.06, 0.2, 1.0], hspace=0.02)

        # title
        ax_title = fig.add_subplot(gs[0])
        ax_title.axis('off')
        title_text = f'{"CPU" if is_time else "Memory"} Overhead Distribution: Sum vs XSan'
        ax_title.text(0.5, 0.5, title_text, ha='center',
                      va='center', fontsize=16, fontweight='bold')

        # legend row
        ax_legend = fig.add_subplot(gs[1])
        ax_legend.axis('off')

        # main axis
        ax = fig.add_subplot(gs[2])

        # --- 3. Draw violins per group ---
        for idx, g in enumerate(groups):
            gdf = df[df["Group"] == g]

            # Sum vs XSan
            for t, hatch, shift in [("Sum", "", -0.2), ("XSan", "///", 0.2)]:
                sdf = gdf[gdf["Type"] == t]
                color = group_colors[g]

                # Draw violin for this group/type
                parts = ax.violinplot(
                    dataset=[sdf["Overhead"]],
                    positions=[idx + shift],
                    widths=0.35,
                    showmedians=True,
                    showmeans=False,
                )
                for pc in parts['bodies']:
                    pc.set_facecolor(color)
                    pc.set_edgecolor("black")
                    pc.set_alpha(0.5)
                    if hatch:
                        pc.set_hatch(hatch)
                for partname in ('cbars', 'cmins', 'cmaxes', 'cmedians'):
                    vp = parts[partname]
                    vp.set_edgecolor("black")
                    vp.set_linewidth(1)

                # Median label
                median_val = sdf["Overhead"].median()
                ax.text(idx + shift,
                        median_val,
                        f"{median_val:.0f}%",
                        ha="center", va="bottom",
                        fontsize=8, fontweight="bold")

        # --- 4. Styling ---
        # baseline
        ax.axhline(y=100, color="red", linestyle="--", linewidth=1.2, zorder=1)

        ax.set_xticks(range(len(groups)))
        ax.set_xticklabels([])
        ax.set_xlim(-0.5, len(groups) - 0.5)
        ax.set_ylabel("Overhead (%)")

        # Legend replaces x-axis labels
        legend_handles = []
        for gi, (sum_name, _) in enumerate(san_pair_to_compare):
            base_color = group_colors[sum_name]
            legend_handles.append(
                Patch(facecolor=base_color, edgecolor="black", linewidth=0.4, alpha=0.65,
                      label=f"{sum_name} (Sum)")
            )
            legend_handles.append(
                Patch(facecolor=base_color, edgecolor="black", linewidth=0.4, alpha=0.65,
                      hatch="///", label=f"{sum_name} (XSan)")
            )
        if legend_handles:
            ncol = min(4, max(1, len(legend_handles)))
            ax_legend.legend(handles=legend_handles,
                             loc='center',
                             ncol=ncol,
                             frameon=False,
                             handlelength=1.2,
                             fontsize=9)

        # Trim surrounding whitespace
        # plt.tight_layout(rect=[0.05, 0.05, 0.98, 0.95])
        typ = 'time' if is_time else 'rss'
        plt.savefig(f'compare_violin_{typ}.{OUTPUT_FORMAT}',
                    dpi=DPI, bbox_inches='tight')
        plt.close()

    # --- run both ---
    draw_bar()
    draw_violin()
    print("------ Compare figures (bar + violin) saved ------")


def plot_figures():
    '''
    plot the figure of the overheads of the sanitizers on the benchmarks
    '''
    plot_figure(True)
    plot_figure(False)


def draw_overall_figures(
    overheads: np.ndarray,
    sanitizers: list,
    ignore_small: bool = False,
    is_rss: bool = False,
):
    '''
    draw the bar chart of the overhead of the sanitizers over the benchmarks
    overheads_over_benchmarks : N * M, N = len(BENCHMARKS), M = len(sanitizers)
    '''
    # Get N and M
    N = overheads.shape[0]  # Number of benchmarks
    M = overheads.shape[1]  # Number of sanitizers

    def draw_bar():
        benchmarks = BENCH_TO_PLOT
        n_bench = len(benchmarks)
        x_indices = np.arange(M)  # M sanitizers for each subplot
        bar_width = 0.8

        # Layout subplots: at most 5 benchmarks per row
        ncols = 5
        nrows = int(np.ceil(n_bench / ncols))

        # Adjust figure size based on row/column count
        fig_width = 3 * ncols
        fig_height = 2.5 * nrows + 2
        fig = plt.figure(figsize=(fig_width, fig_height))
        gs = fig.add_gridspec(
            nrows + 2, ncols,
            height_ratios=[0.1, 0.2] + [1.0] * nrows,
            hspace=0.35, wspace=0.25
        )

        # --- Title row ---
        ax_title = fig.add_subplot(gs[0, :])
        ax_title.axis("off")
        title_text = f'{"CPU" if not is_rss else "Memory"} Overheads of Sanitizers on Benchmarks{" (ignore small)" if ignore_small else ""}'
        ax_title.text(0.5, 0.5, title_text,
                      ha="center", va="center",
                      fontsize=14, fontweight="bold")

        # Legend row
        ax_legend = fig.add_subplot(gs[1, :])
        ax_legend.axis("off")

        linewidth = 0.4
        colors = plt.get_cmap("tab20").colors

        # One subplot per benchmark
        for bi, bench in enumerate(benchmarks):
            row = bi // ncols + 2
            col = bi % ncols
            ax = fig.add_subplot(gs[row, col])

            bars = []
            for j in range(M):
                ax.bar(
                    x_indices[j],
                    overheads[bi, j],
                    width=bar_width * 0.9,
                    label=sanitizers[j],
                    color=colors[j % len(colors)],
                    edgecolor="black",
                    linewidth=linewidth,
                    alpha=0.9
                )

            # baseline
            ax.axhline(y=100, color="red", linestyle="--", linewidth=1)

            # Title & axes
            ax.set_title(bench, fontsize=10)
            ax.set_xticks([])
            if col == 0:
                ax.set_ylabel("Overhead (%)")

        # Legend
        legend_handles = [
            plt.Rectangle((0, 0), 1, 1,
                          facecolor=colors[j % len(colors)],
                          edgecolor="black",
                          linewidth=linewidth,
                          label=sanitizers[j])
            for j in range(M)
        ]
        ncol = min(6, M)
        ax_legend.legend(
            handles=legend_handles,
            labels=sanitizers,
            loc="center",
            ncol=ncol,
            frameon=False,
            fontsize=9
        )

        # Save figure
        typ = 'rss' if is_rss else 'time'
        typ = f'{typ}_big' if ignore_small else typ
        plt.savefig(f'overheads_{typ}.{OUTPUT_FORMAT}',
                    dpi=DPI, bbox_inches='tight')
        plt.close(fig)

    def draw_violin():
        # --- 1. Prepare data ---
        # overheads: shape (N_benchmarks, M_sanitizers) or similar;
        # we treat each column as a sanitizer distribution.
        df = pd.DataFrame(overheads, columns=sanitizers)

        # --- 2. Color mapping ---
        cmap = plt.get_cmap("tab20")
        colors = [cmap(i % 20) for i in range(len(sanitizers))]

        fig = plt.figure(figsize=(10, 10))
        gs = fig.add_gridspec(3, 1, height_ratios=[
                              0.08, 0.14, 1.0], hspace=0.05)

        # title
        ax_title = fig.add_subplot(gs[0])
        ax_title.axis("off")
        title_text = (
            "CPU" if not is_rss else "Memory"
        ) + " Overhead Distribution of Sanitizers"
        if ignore_small:
            title_text += " (ignore small)"
        ax_title.text(
            0.5,
            0.5,
            title_text,
            ha="center",
            va="center",
            fontsize=16,
            fontweight="bold",
        )

        # legend row
        ax_legend = fig.add_subplot(gs[1])
        ax_legend.axis("off")

        # main axis
        ax = fig.add_subplot(gs[2])

        # --- 3. Draw violin per sanitizer ---
        legend_handles = []
        for idx, san in enumerate(sanitizers):
            sdf = df[san].dropna()
            parts = ax.violinplot(
                dataset=[sdf],
                positions=[idx],
                widths=0.6,
                showmedians=True,
                showmeans=False,
            )
            # Color each violin
            for pc in parts["bodies"]:
                pc.set_facecolor(colors[idx])
                pc.set_edgecolor("black")
                pc.set_alpha(0.7)
            for partname in ("cbars", "cmins", "cmaxes", "cmedians"):
                vp = parts[partname]
                vp.set_edgecolor("black")
                vp.set_linewidth(1)

            # Median label
            median_val = sdf.median()
            ax.text(
                idx,
                median_val,
                f"{median_val:.0f}%",
                ha="center",
                va="bottom",
                fontsize=8,
                fontweight="bold",
            )

            # legend handle
            legend_handles.append(
                Patch(
                    facecolor=colors[idx],
                    edgecolor="black",
                    linewidth=0.6,
                    label=san,
                )
            )

        # --- 4. Styling ---
        ax.axhline(y=100, color="red", linestyle="--", linewidth=1.2, zorder=1)
        ax.set_xticks(range(len(sanitizers)))
        ax.set_xticklabels([])
        ax.set_xlim(-0.5, len(sanitizers) - 0.5)
        ax.set_ylabel("Overhead (%)")

        # legend
        ncol = min(6, max(1, len(legend_handles)))
        ax_legend.legend(
            handles=legend_handles,
            loc="center",
            ncol=ncol,
            frameon=False,
            handlelength=1.2,
            fontsize=9,
        )

        # Save figure
        typ = "rss" if is_rss else "time"
        typ = f"{typ}_big" if ignore_small else typ
        plt.savefig(f"overheads_violin_{typ}.{OUTPUT_FORMAT}",
                    dpi=DPI, bbox_inches="tight")
        plt.close(fig)

    draw_bar()
    draw_violin()
    print('--------------- Done! ----------------')


def main():
    plot_figures()


if __name__ == '__main__':
    if len(sys.argv) > 1:
        FORCE = sys.argv[1] == '-f'
    main()
