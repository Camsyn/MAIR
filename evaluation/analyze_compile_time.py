import re
import json
from collections import defaultdict
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

def extract_compile_time(file_path):
    """Extract compile time (seconds) from a file."""
    try:
        with open(file_path, 'r') as f:
            content = f.read()
            match = re.search(r'Compile Time: (\d+) s', content)
            if match:
                return int(match.group(1))
            else:
                print(f"Warning: Compile time not found in file {file_path}")
                return None
    except Exception as e:
        print(f"Error reading file {file_path}: {e}")
        return None

def sum_executecompiler_dur_json(path: Path):
    """
    Traverse all *.json files in the path directory, find all events in the
    traceEvents list with name 'ExecuteCompiler', and sum their 'dur'.
    dur unit: microseconds (us), returns (total sum, count)
    """
    total_dur = 0
    found_count = 0
    durations = []
    for json_file in path.rglob("*.json"):
        try:
            with open(json_file, "r") as f:
                obj = json.load(f)
            if isinstance(obj, dict) and "traceEvents" in obj and isinstance(obj["traceEvents"], list):
                events = obj["traceEvents"]
            else:
                continue
            for event in events:
                if isinstance(event, dict) and event.get("name") == "ExecuteCompiler":
                    dur = event.get("dur")
                    if dur is not None:
                        try:
                            dur_f = float(dur)
                            durations.append((dur_f, json_file.name))
                            total_dur += dur_f
                            found_count += 1
                        except Exception:
                            continue
        except Exception as e:
            print(f"Error reading JSON file {json_file}: {e}")
    return total_dur, found_count, durations

def plot_compile_trace_grid_bar(results, show_targets,
                                outfile="compile_trace_grid_bar.pdf"):
    """
    Plot compile trace percentage for each package (3x7 grid; shaded bars for Sum and XSan; compact legend).

    Changelog:
    1. Legend text uses \\textsc (legend label uses TeX format string)
    2. Default output is PDF (vector graphics), file suffix is customizable
    3. Compress each subplot width to fit single column
    4. Increase font size for subplot titles and y-axis labels
    5. Automatically set ylim per subplot to ensure top text is fully inside the plot
    """

    # -------------- Basic Parameters --------------
    ncols = 7
    nrows = 3
    n_pkg = len(results)
    show_targets = list(show_targets)
    packages = [r["package"] for r in results]

    target_idx = {name: i for i, name in enumerate(show_targets)}

    # Legend / label uses \textsc
    xlabels = [x[:2].upper() + x[2:] for x in show_targets]
    if "xsan-asan-msan-tsan" in show_targets:
        xlabels[target_idx["xsan-asan-msan-tsan"]] = "XSan"
        xlabels[target_idx["sum"]] = "Sum"
    # xlabels = [r"$\textsc{" + x + "}$" for x in xlabels]

    idx_sum = target_idx.get("sum", len(show_targets) - 2)
    idx_xsan = target_idx.get("xsan-asan-msan-tsan", len(show_targets) - 1)

    # -------------- Figure Size (narrow subplot width for single column) --------------
    # Originally 2.7 * ncols, here reduced to about 2.0 * ncols, can be tuned for actual journal width
    figsize = (1.2 * ncols, 2.0 * (nrows + 0.63))

    fig = plt.figure(figsize=figsize)
    gs = fig.add_gridspec(
        nrows + 1,
        ncols,
        height_ratios=[0.09] + [1.0] * nrows,  # Row 0 is the legend
        hspace=0.3,
        wspace=0.55,  # Horizontal spacing between subplots
    )

    # -------------- Legend Axis --------------
    ax_legend = fig.add_subplot(gs[0, :])
    ax_legend.axis("off")

    cmap = plt.get_cmap("tab20")
    colors = [cmap(i) for i in range(len(xlabels))]
    edgecolor = "black"
    linewidth = 0.2
    alpha = 0.9
    hatch_sum = r"////"
    hatch_xsan = r"\\\\"

    # -------------- Each Subplot --------------
    title_fs = 11        # Larger subplot title
    ylabel_fs = 13       # Larger y-axis label font
    ytick_fs = 9
    legend_fs = 13

    label_height = 10
    for idx in range(nrows * ncols):
        row, col = divmod(idx, ncols)
        ax = fig.add_subplot(gs[row + 1, col])  # row+1: row 0 is the legend

        if idx >= n_pkg:
            ax.axis("off")
            continue

        res = results[idx]
        yvals = [
            res["trace_percent"][t] if res["trace_percent"][t] is not None else 0
            for t in show_targets
        ]
        max_y = max(yvals) if yvals else 0

        # ---- Calculate text offset and upper y limit based on current subplot's max value ----
        # The vertical offset of the text atop the bar; min 5, increases slightly with max_y
        text_offset = max(5.0, 0.03 * max(110.0, float(max_y)))
        # The highest position of the text will not exceed max_y + text_offset
        text_top = max_y + text_offset
        # Leave some headroom above ylim top to ensure text is fully inside the plot
        upper_lim = max(150.0, text_top + max(5.0, 0.02 * float(max_y)) + label_height)

        for j, y in enumerate(yvals):
            hatch = None
            if j == idx_sum:
                hatch = hatch_sum
            elif j == idx_xsan:
                hatch = hatch_xsan

            ax.bar(
                j,
                y,
                color=colors[j],
                edgecolor=edgecolor,
                linewidth=linewidth,
                alpha=alpha,
                hatch=hatch if hatch else None,
                zorder=3,
            )

            # # Text atop bar (ensure fully inside plot with the above upper_lim)
            # if res["trace_percent"][show_targets[j]] is not None:
            #     ax.text(
            #         j,
            #         y + text_offset,
            #         f"{y:.0f}%",
            #         ha="center",
            #         va="bottom",
            #         fontsize=label_height,
            #     )

        ax.set_xticks(range(len(xlabels)))
        # Leave x ticks blank, show labels only in the legend
        ax.set_xticklabels([""] * len(xlabels), rotation=17, fontsize=10)

        ax.set_title(res["package"], fontsize=title_fs)
        ax.axhline(100, ls="--", color="red", lw=1, zorder=100)

        ax.set_ylim(0, upper_lim)

        if col == 0:
            ax.set_ylabel("Ratio (%)", fontsize=ylabel_fs)
        else:
            ax.set_ylabel("")

        ax.tick_params(axis="y", labelsize=ytick_fs)
        ax.tick_params(axis="x", length=2.3)

    # -------------- Legend (compact + \textsc) --------------
    legend_patches = []
    for i, xlab in enumerate(xlabels):
        if i == idx_sum:
            legend_patches.append(
                mpatches.Patch(
                    facecolor=colors[i],
                    edgecolor=edgecolor,
                    linewidth=linewidth,
                    hatch=hatch_sum,
                    label=xlab,
                )
            )
        elif i == idx_xsan:
            legend_patches.append(
                mpatches.Patch(
                    facecolor=colors[i],
                    edgecolor=edgecolor,
                    linewidth=linewidth,
                    hatch=hatch_xsan,
                    label=xlab,
                )
            )
        else:
            legend_patches.append(
                mpatches.Patch(
                    facecolor=colors[i],
                    edgecolor=edgecolor,
                    linewidth=linewidth,
                    label=xlab,
                )
            )
    # Modify legend's bbox_to_anchor to center the legend
    ax_legend.legend(
        handles=legend_patches,
        loc="center",
        ncol=len(xlabels),
        fontsize=legend_fs,      # Slightly larger legend font size
        frameon=False,
        handlelength=1.26,
        borderaxespad=0.06,
        bbox_to_anchor=(0.5, 0.5),  # Fully centered
    )

    # -------------- Save as PDF --------------
    # As long as outfile has .pdf extension it will save as a vector graphic; can also explicitly set format="pdf"
    plt.savefig(outfile, bbox_inches="tight", format="pdf")
    print(f"\nFigure saved to {outfile}")

def main():
    package_data = defaultdict(dict)
    package_dur_data = defaultdict(dict)

    current_dir = Path('.')
    for item in current_dir.iterdir():
        if item.is_dir() and not item.name.startswith('.'):
            package = item.name
            compiled_dir = item / '.compiled'
            data_dir = compiled_dir / 'data-time-trace'
            if compiled_dir.is_dir():
                for t in compiled_dir.iterdir():
                    if t.is_file():
                        compile_time = extract_compile_time(t)
                        if compile_time is not None:
                            package_data[package][t.name] = compile_time
            if data_dir.is_dir():
                for t in data_dir.iterdir():
                    if t.is_dir():
                        dur_total, count_found, durations = sum_executecompiler_dur_json(t)
                        if count_found > 0:
                            package_dur_data[package][t.name] = {
                                "duration_us": dur_total, "count": count_found
                            }
    # Remove 'raw' column, only keep 'asan','msan','tsan','sum','xsan-asan-msan-tsan'
    show_targets = ['asan', 'msan', 'tsan', 'sum', 'xsan-asan-msan-tsan']
    base_required = ['raw', 'asan', 'msan', 'tsan', 'xsan-asan-msan-tsan']

    results = []
    for package, targets in package_data.items():
        missing = [t for t in base_required if t not in targets]
        if missing:
            print(f"Warning: package {package} is missing target(s): {', '.join(missing)}, skip")
            continue

        try:
            raw_time = targets['raw']
            asan_time = targets['asan']
            msan_time = targets['msan']
            tsan_time = targets['tsan']
            xsan_time = targets['xsan-asan-msan-tsan']
        except Exception as e:
            print(f"Data error for package {package}: {e}, skipping")
            continue

        if raw_time == 0:
            print(f"Warning: package {package} has raw time 0, skip")
            continue

        xsan_all_overhead = xsan_time / raw_time
        asan_overhead = asan_time / raw_time
        msan_overhead = msan_time / raw_time
        tsan_overhead = tsan_time / raw_time
        sum_overhead = asan_overhead + msan_overhead + tsan_overhead
        improvement = (sum_overhead - xsan_all_overhead) / sum_overhead * 100 if sum_overhead != 0 else 0

        # compile trace json
        json_trace_times = {}
        for t in ['raw', 'asan', 'msan', 'tsan', 'xsan-asan-msan-tsan']:
            dur_entry = package_dur_data.get(package, {}).get(t)
            if dur_entry:
                seconds = dur_entry['duration_us'] / 1e6
                json_trace_times[t] = seconds
            else:
                json_trace_times[t] = None
        # sum trace
        if all(json_trace_times[x] is not None for x in ('asan','msan','tsan')):
            sum_trace = json_trace_times['asan'] + json_trace_times['msan'] + json_trace_times['tsan']
        else:
            sum_trace = None
        json_trace_times['sum'] = sum_trace

        # Percentage data, relative to raw
        raw_trace = json_trace_times['raw']
        trace_percent = {}
        for t in show_targets:
            v = json_trace_times.get(t)
            if v is None or raw_trace in (None,0):
                trace_percent[t] = None
            else:
                trace_percent[t] = v / raw_trace * 100

        results.append({
            'package': package,
            'xsan_all_overhead': xsan_all_overhead,
            'sum_overhead': sum_overhead,
            'improvement': improvement,
            'asan_overhead': asan_overhead,
            'msan_overhead': msan_overhead,
            'tsan_overhead': tsan_overhead,
            'json_trace_times': dict(json_trace_times),
            'trace_percent': dict(trace_percent),
        })

    if not results:
        print("No valid data available for analysis")
        return

    print("\n===== Compile Trace Tracking (Percentage relative to Raw) =====")
    print(f"{'Package':<15}", end='')
    for t in show_targets:
        print(f"{t:<15}", end='')
    print()
    print('-' * (15 + len(show_targets)*15))
    for res in results:
        print(f"{res['package']:<15}", end='')
        for t in show_targets:
            v = res['trace_percent'][t]
            if v is not None:
                print(f"{v:.2f}%{'':<8}", end='')
            else:
                print(f"{'--':<15}", end='')
        print()
    print('-' * (15 + len(show_targets)*15))
    print(f"{'Average':<15}", end='')
    for t in show_targets:
        vals = [r['trace_percent'][t] for r in results if r['trace_percent'][t] is not None]
        if vals:
            v = sum(vals) / len(vals)
            print(f"{v:.2f}%{'':<8}", end='')
        else:
            print(f"{'--':<15}", end='')
    print()

    plot_compile_trace_grid_bar(results, show_targets, outfile="compile_trace_grid_bar.pdf")


if __name__ == "__main__":
    main()
