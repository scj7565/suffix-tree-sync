"""Make plots and a summary table from run_all.bat results (CSV). Owner: lazy.

  python scripts/plot.py                              # results/all_scale.csv, all_stress.csv, all_mixed.csv
  python scripts/plot.py results/quick_scale.csv      # give CSV files directly (one or more)
  python scripts/plot.py --out results/plots --at 8   # output folder, thread count for plots 3 and 5

Requires: pip install pandas matplotlib

Output (--out folder)
  1_step_ms.png      threads -> step time (rows = dataset, columns = evictions per step). Main result
  2_speedup.png      threads -> speedup over serial with 1 thread
  3_breakdown.png    time per phase at --at threads (local update, global update, remove, speculate, end), stacked
  4_lockwait.png     threads -> share of time spent waiting for locks
  5_evict.png        evictions per step -> step time at --at threads (stress-test levels marked "stress")
  6_mixed.png        threads -> step time for the mixed schedule (when a mixed CSV is given)
  summary.md         per mode: 1-thread time, best time and its thread count, speedup over serial 1 thread

Rows from a file whose name contains "stress" (run_all.bat stage 3: many evictions per step) are a stress
test. They are kept out of plots 1-4 and only appear in plot 5 and the summary, labeled as stress.

If the same condition (dataset, mode, schedule, threads, settings) appears more than once, the last row wins
(a re-measurement). Each mode keeps a fixed color in every plot (serial, global_lock, subtree_lock, node_lock,
optimistic, lazy in that order; other modes take the remaining slots).
"""
import argparse
import pathlib
import sys

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import pandas as pd
except ImportError:
    sys.exit("pandas and matplotlib are required:  pip install pandas matplotlib")

ROOT = pathlib.Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------- style
# 8 categorical colors in a fixed order (palette checked for color-vision-deficiency separation).
# A mode has the same color in every plot.
PALETTE = ["#257de0", "#f47230", "#00b894", "#f2a600", "#ee7fac", "#008a17", "#6a4ce2", "#ec4647"]
MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]   # shape too, so identity never depends on color alone
KNOWN_MODES = ["serial", "global_lock", "subtree_lock", "node_lock", "optimistic", "lazy"]
# Phases happen in a fixed order -> one blue hue, light to dark (ordinal)
PHASES = ["ph_local_ms", "ph_global_ms", "ph_remove_ms", "ph_spec_ms", "ph_end_ms"]
PHASE_COLORS = ["#84b9f3", "#519ced", "#257de0", "#1b64be", "#0e3d7e"]
TEXT, TEXT2, GRID, SURFACE = "#191919", "#666666", "#e3e3e3", "#ffffff"
KEY = ["data", "mode", "sched", "threads", "B", "N_CACHED", "STEPS", "EVICT", "local_par"]

L = {
    "threads": "threads", "step_ms": "step time (ms, lower is better)", "speedup": "x vs serial 1 thread",
    "lockwait": "lock wait (%)", "evict": "responses evicted per step", "ms": "step time (ms)",
    "phases": ["local tree update", "global tree update", "remove", "speculate", "end (step cleanup)"],
    "t1": "Step time vs threads (phase)", "t2": "Speedup (serial 1 thread = 1)",
    "t3": "Time per phase at {t} threads", "t4": "Lock wait", "t5": "Step time vs evictions at {t} threads",
    "t6": "Reads and writes mixed (mixed)", "evict_n": "evict {e}",
    "note3": "Modes with caps par_remove=true run removals inside the global update phase, "
             "so their removal time is counted in 'global tree update'.",
}


def style_axes(ax):
    ax.set_facecolor(SURFACE)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=TEXT2, labelsize=8)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def mode_styles(modes):
    """mode -> (color, marker, line style). Known modes keep fixed slots, others take free slots by name."""
    order = [m for m in KNOWN_MODES if m in modes] + sorted(m for m in modes if m not in KNOWN_MODES)
    slots = {m: KNOWN_MODES.index(m) for m in order if m in KNOWN_MODES}
    free = [i for i in range(len(PALETTE)) if i not in slots.values()]
    for m in order:
        if m not in slots:
            slots[m] = free.pop(0) if free else None
    styles = {}
    for m in order:
        i = slots[m]
        if i is None:   # more than 8 modes: no new hues, gray dashed instead (better: plot fewer modes)
            print(f"[WARN] more than 8 modes: '{m}' is drawn as a gray dashed line")
            styles[m] = ("#9c9c9c", "x", "--")
        else:
            styles[m] = (PALETTE[i], MARKERS[i], "-")
    return order, styles


def load(paths):
    frames = []
    for p in paths:
        p = pathlib.Path(p)
        if not p.exists():
            continue
        df = pd.read_csv(p)
        df["stress"] = "stress" in p.name
        frames.append(df)
    if not frames:
        return None
    df = pd.concat(frames, ignore_index=True)
    df["data"] = df["data"].astype(str).str.replace(r"^tokens_", "", regex=True).str.replace(r"\.bin$", "", regex=True)
    df = df.drop_duplicates(subset=KEY, keep="last")   # a re-measured condition: keep the last value
    return df


def mode_handles(order, styles):
    return [plt.Line2D([], [], color=styles[m][0], marker=styles[m][1], linestyle=styles[m][2],
                       linewidth=2, markersize=6, markeredgecolor=SURFACE) for m in order]


def finish(fig, title, handles, labels, out, note=None):
    """Title on top, legend under it, plots below; margins fixed in inches. Then save."""
    h = fig.get_figheight()
    top_in = 0.75                      # title + legend height (inches)
    bottom_in = 0.35 if note else 0.0
    fig.suptitle(title, y=1 - 0.12 / h, fontsize=11, color=TEXT, va="top")
    fig.legend(handles, labels, loc="upper center", ncol=min(len(labels), 6), frameon=False,
               bbox_to_anchor=(0.5, 1 - 0.38 / h), fontsize=9, labelcolor=TEXT)
    if note:
        fig.text(0.01, 0.08 / h, note, fontsize=7.5, color=TEXT2, ha="left", va="bottom")
    fig.tight_layout(rect=(0, bottom_in / h, 1, 1 - top_in / h))
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    print("saved", out)


def grid_figure(df, value, ylabel, title, order, styles, out, transform=None, hline=None):
    """rows = dataset, columns = evictions, lines = modes, x = threads."""
    datas, evicts = sorted(df["data"].unique()), sorted(df["EVICT"].unique())
    fig, axes = plt.subplots(len(datas), len(evicts), figsize=(3.6 * len(evicts), 2.9 * len(datas) + 0.8),
                             squeeze=False)
    for r, d in enumerate(datas):
        row_max = 0.0
        for c, e in enumerate(evicts):
            ax = axes[r][c]
            style_axes(ax)
            sub = df[(df["data"] == d) & (df["EVICT"] == e)]
            for m in order:
                s = sub[sub["mode"] == m].sort_values("threads")
                if s.empty:
                    continue
                y = transform(s, sub) if transform else s[value]
                if y is None:
                    continue
                col, mk, ls = styles[m]
                ax.plot(s["threads"], y, color=col, marker=mk, linestyle=ls, linewidth=2, markersize=6,
                        markeredgecolor=SURFACE, markeredgewidth=1)
                row_max = max(row_max, float(pd.Series(y).max()))
            if hline is not None:
                ax.axhline(hline, color=TEXT2, linewidth=1, linestyle=":")
            ax.set_xticks(sorted(sub["threads"].unique()))
            ax.set_title(f"{d} · {L['evict_n'].format(e=e)}", fontsize=9, color=TEXT)
            if r == len(datas) - 1:
                ax.set_xlabel(L["threads"], fontsize=8, color=TEXT2)
            if c == 0:
                ax.set_ylabel(ylabel, fontsize=8, color=TEXT2)
        top = max(row_max, hline or 0) * 1.08 or 1.0
        for c in range(len(evicts)):   # one scale per row, so eviction levels compare directly
            axes[r][c].set_ylim(0, top)
    finish(fig, title, mode_handles(order, styles), order, out)


def pick_threads(df, want):
    ts = sorted(df["threads"].unique())
    below = [t for t in ts if t <= want]
    return below[-1] if below else ts[-1]


def breakdown_figure(df, at, order, out):
    datas, evicts = sorted(df["data"].unique()), sorted(df["EVICT"].unique())
    t = pick_threads(df, at)
    sub_all = df[df["threads"] == t]
    fig, axes = plt.subplots(len(datas), len(evicts), figsize=(3.6 * len(evicts), 3.0 * len(datas) + 1.0),
                             squeeze=False)
    for r, d in enumerate(datas):
        row_tops = []
        for c, e in enumerate(evicts):
            ax = axes[r][c]
            style_axes(ax)
            sub = sub_all[(sub_all["data"] == d) & (sub_all["EVICT"] == e)].set_index("mode")
            ms = [m for m in order if m in sub.index]
            bottom = [0.0] * len(ms)
            for p, col in zip(PHASES, PHASE_COLORS):
                vals = [float(sub.loc[m, p]) for m in ms]
                ax.bar(range(len(ms)), vals, bottom=bottom, color=col, width=0.7,
                       edgecolor=SURFACE, linewidth=1)   # 2px gap between segments
                bottom = [b + v for b, v in zip(bottom, vals)]
            ax.set_xticks(range(len(ms)))
            ax.set_xticklabels(ms, rotation=30, ha="right", fontsize=8)
            ax.set_title(f"{d} · {L['evict_n'].format(e=e)}", fontsize=9, color=TEXT)
            if c == 0:
                ax.set_ylabel(L["ms"], fontsize=8, color=TEXT2)
            row_tops.append(max(bottom) if bottom else 0.0)
        for c in range(len(evicts)):
            axes[r][c].set_ylim(0, (max(row_tops) or 1.0) * 1.08)
    handles = [plt.Rectangle((0, 0), 1, 1, color=c) for c in PHASE_COLORS]
    finish(fig, L["t3"].format(t=t), handles, L["phases"], out, note=L["note3"])


def evict_figure(df, at, order, styles, out):
    datas = sorted(df["data"].unique())
    t = pick_threads(df[~df["stress"]] if (~df["stress"]).any() else df, at)
    sub_all = df[df["threads"] == t]
    if (df["stress"].any() and not sub_all["stress"].any()):
        print(f"[WARN] stress rows have no {t}-thread result; plot 5 shows the main levels only")
    if sub_all["EVICT"].nunique() < 2:
        print("skip", out, "(only one eviction level)")
        return
    stress_levels = set(sub_all.loc[sub_all["stress"], "EVICT"])
    fig, axes = plt.subplots(1, len(datas), figsize=(4.2 * len(datas), 3.6), squeeze=False)
    for c, d in enumerate(datas):
        ax = axes[0][c]
        style_axes(ax)
        sub = sub_all[sub_all["data"] == d]
        for m in order:
            s = sub[sub["mode"] == m].sort_values("EVICT")
            if s.empty:
                continue
            col, mk, ls = styles[m]
            ax.plot(s["EVICT"], s["step_ms"], color=col, marker=mk, linestyle=ls, linewidth=2, markersize=6,
                    markeredgecolor=SURFACE, markeredgewidth=1)
        ticks = sorted(sub["EVICT"].unique())
        ax.set_xticks(ticks)
        ax.set_xticklabels([f"{e}\n(stress)" if e in stress_levels else str(e) for e in ticks])
        ax.set_title(d, fontsize=9, color=TEXT)
        ax.set_xlabel(L["evict"], fontsize=8, color=TEXT2)
        if c == 0:
            ax.set_ylabel(L["step_ms"], fontsize=8, color=TEXT2)
        ax.set_ylim(bottom=0)
    finish(fig, L["t5"].format(t=t), mode_handles(order, styles), order, out)


def speedup(s, sub):
    base = sub[(sub["mode"] == "serial") & (sub["threads"] == 1)]
    if base.empty:
        return None
    return float(base["step_ms"].iloc[0]) / s["step_ms"]


def summary(df, order, out):
    lines = ["| data | evict | mode | 1-thread ms | best ms (threads) | vs serial 1 thread | lock wait % (at best) |",
             "|---|---|---|---|---|---|---|"]
    for d in sorted(df["data"].unique()):
        for e in sorted(df["EVICT"].unique()):
            sub = df[(df["data"] == d) & (df["EVICT"] == e)]
            base = sub[(sub["mode"] == "serial") & (sub["threads"] == 1)]
            b = float(base["step_ms"].iloc[0]) if not base.empty else None
            for m in order:
                s = sub[sub["mode"] == m]
                if s.empty:
                    continue
                one = s[s["threads"] == 1]
                best = s.loc[s["step_ms"].idxmin()]
                one_ms = f"{float(one['step_ms'].iloc[0]):.3f}" if not one.empty else "-"
                x = f"x{b / best['step_ms']:.2f}" if b else "-"
                ev = f"{e} (stress)" if bool(s["stress"].any()) else str(e)
                lines.append(f"| {d} | {ev} | {m} | {one_ms} | {best['step_ms']:.3f} ({int(best['threads'])}) | {x} | "
                             f"{best['lockwait_pct']:.1f} |")
    text = "\n".join(lines) + "\n"
    pathlib.Path(out).write_text(text, encoding="utf-8")
    print(text)
    print("saved", out)


def main():
    ap = argparse.ArgumentParser(description="Plots from run_all.bat results")
    ap.add_argument("csv", nargs="*", help="CSV files (default: results/all_scale.csv, results/all_mixed.csv)")
    ap.add_argument("--out", default=str(ROOT / "results" / "plots"), help="output folder")
    ap.add_argument("--at", type=int, default=8,
                    help="thread count for the phase and eviction plots (falls back to the largest one below it)")
    a = ap.parse_args()
    paths = a.csv or [ROOT / "results" / "all_scale.csv", ROOT / "results" / "all_stress.csv",
                      ROOT / "results" / "all_mixed.csv"]
    df = load(paths)
    if df is None or df.empty:
        sys.exit(f"no CSV to read: {[str(p) for p in paths]}  (run scripts\\run_all.bat first)")
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    order, styles = mode_styles(set(df["mode"]))

    ph_all = df[df["sched"] == "phase"]
    ph = ph_all[~ph_all["stress"]]             # main measurement only (stress test kept out of plots 1-4)
    if ph.empty:
        ph = ph_all                            # only stress rows were given: plot them anyway
    if not ph.empty:
        grid_figure(ph, "step_ms", L["step_ms"], L["t1"], order, styles, out / "1_step_ms.png")
        if (ph["mode"] == "serial").any():
            grid_figure(ph, None, L["speedup"], L["t2"], order, styles, out / "2_speedup.png",
                        transform=speedup, hline=1.0)
        else:
            print("skip 2_speedup.png (no serial results)")
        breakdown_figure(ph, a.at, order, out / "3_breakdown.png")
        grid_figure(ph, "lockwait_pct", L["lockwait"], L["t4"], order, styles, out / "4_lockwait.png")
        evict_figure(ph_all, a.at, order, styles, out / "5_evict.png")
        summary(ph_all, order, out / "summary.md")
    mx = df[df["sched"] == "mixed"]
    if not mx.empty:
        grid_figure(mx, "step_ms", L["step_ms"], L["t6"], order, styles, out / "6_mixed.png")


if __name__ == "__main__":
    main()
