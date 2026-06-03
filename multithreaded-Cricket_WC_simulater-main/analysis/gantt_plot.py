#!/usr/bin/env python3
"""
gantt_plot.py — Generate full-match thread/resource charts from simulator CSV logs.
Supports one or two innings per CSV and can batch-process FCFS/SJF/Priority logs.
"""

import argparse
import os
import sys
from pathlib import Path

try:
    import pandas as pd
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
except ImportError:
    print("ERROR: Install dependencies with: pip install matplotlib pandas")
    sys.exit(1)


DEFAULT_COLOUR = "#95a5a6"
PHASE_COLOURS = {
    "Powerplay": "#27ae60",
    "Middle": "#2980b9",
    "Middle-late": "#8e44ad",
    "Death": "#c0392b",
}
EVENT_COLOURS = {
    "BALL_BOWLED": "#3498db",
    "WIDE": "#e67e22",
    "NO_BALL": "#e67e22",
    "FREE_HIT": "#9b59b6",
    "BALL_IN_AIR": "#f39c12",
    "RUNS_SCORED": "#2ecc71",
    "FOUR": "#1abc9c",
    "SIX": "#e74c3c",
    "BATSMAN_OUT": "#c0392b",
    "OVER_COMPLETE": "#7f8c8d",
    "MATCH_OVER": "#2c3e50",
}
MODE_DEFAULTS = {
    "fcfs": "../logs/events_fcfs.csv",
    "sjf": "../logs/events_sjf.csv",
    "priority": "../logs/events_priority.csv",
}


def assign_innings(df: pd.DataFrame) -> pd.Series:
    innings = []
    current = 1
    for evt in df["event_type"]:
        innings.append(current)
        if evt == "MATCH_OVER":
            current += 1
    return pd.Series(innings, index=df.index, dtype="int64")


def load_events(csv_path: str) -> pd.DataFrame:
    if not os.path.exists(csv_path):
        raise FileNotFoundError(f"{csv_path} not found")

    df = pd.read_csv(csv_path)
    df.columns = df.columns.str.strip()
    df["timestamp_us"] = pd.to_numeric(df["timestamp_us"], errors="coerce")
    df.dropna(subset=["timestamp_us"], inplace=True)
    df = df.reset_index(drop=True)
    df["innings"] = assign_innings(df)
    df["innings_label"] = df["innings"].map(lambda n: f"Innings {n}")
    df["t_sec"] = 0.0

    offset_sec = 0.0
    for innings_no in sorted(df["innings"].unique()):
        mask = df["innings"] == innings_no
        inning_ts = df.loc[mask, "timestamp_us"]
        local_sec = (inning_ts - inning_ts.min()) / 1_000_000
        df.loc[mask, "t_sec"] = local_sec + offset_sec
        offset_sec = float(df.loc[mask, "t_sec"].max()) + 1.0

    for col in ["bowler_name", "batsman_name", "fielder_name", "dismissal_type", "message"]:
        if col in df.columns:
            df[col] = df[col].fillna("").astype(str).replace("—", "").str.strip()
    return df


def build_bowler_palette(df: pd.DataFrame) -> dict:
    names = [name for name in df["bowler_name"].dropna().unique().tolist() if name]
    cmap = plt.get_cmap("tab20")
    return {name: cmap(i % 20) for i, name in enumerate(sorted(names))}


def innings_summaries(df: pd.DataFrame) -> list[dict]:
    summaries = []
    for innings_no, inning_df in df.groupby("innings", sort=True):
        final_runs = int(inning_df["total_runs"].max()) if not inning_df.empty else 0
        final_wkts = int(inning_df["total_wickets"].max()) if not inning_df.empty else 0
        balls = len(inning_df[inning_df["event_type"] == "BALL_BOWLED"])
        overs = f"{balls // 6}.{balls % 6}"
        target = ""
        over_rows = inning_df[inning_df["event_type"] == "MATCH_OVER"]
        result_msg = over_rows["message"].iloc[-1] if not over_rows.empty else ""
        summaries.append(
            {
                "innings": innings_no,
                "score": f"{final_runs}/{final_wkts}",
                "overs": overs,
                "events": len(inning_df),
                "result_msg": result_msg,
                "target": target,
            }
        )
    return summaries


def plot_bowler_gantt(df: pd.DataFrame, ax: plt.Axes, palette: dict) -> None:
    bowled = df[df["event_type"] == "BALL_BOWLED"].copy()
    if bowled.empty:
        ax.text(0.5, 0.5, "No BALL_BOWLED events found", ha="center", va="center", transform=ax.transAxes)
        return

    rows = []
    for innings_no, inning_df in bowled.groupby("innings", sort=True):
        for over in sorted(inning_df["over"].unique()):
            rows.append((innings_no, int(over)))

    y_map = {key: idx for idx, key in enumerate(rows)}
    legend_seen = set()

    for innings_no, over in rows:
        ov_df = bowled[(bowled["innings"] == innings_no) & (bowled["over"] == over)]
        if ov_df.empty:
            continue
        y = y_map[(innings_no, over)]
        bowler = ov_df["bowler_name"].mode().iloc[0] if not ov_df["bowler_name"].mode().empty else "Unknown"
        t_start = ov_df["t_sec"].min()
        t_end = ov_df["t_sec"].max()
        over_end = df[
            (df["innings"] == innings_no)
            & (df["event_type"].isin(["OVER_COMPLETE", "MATCH_OVER"]))
            & (df["t_sec"] >= t_start)
        ]
        if not over_end.empty:
            t_end = max(t_end, float(over_end["t_sec"].iloc[0]))

        phase = ov_df["match_phase"].iloc[0] if "match_phase" in ov_df.columns else ""
        if phase in PHASE_COLOURS:
            ax.axhspan(y - 0.42, y + 0.42, color=PHASE_COLOURS[phase], alpha=0.06, zorder=0)

        ax.barh(
            y,
            max(t_end - t_start, 0.001),
            left=t_start,
            height=0.6,
            color=palette.get(bowler, DEFAULT_COLOUR),
            edgecolor="white",
            linewidth=0.5,
            alpha=0.9,
            label=bowler if bowler not in legend_seen else "",
        )
        legend_seen.add(bowler)
        ax.text((t_start + t_end) / 2, y, bowler.split(".")[-1], ha="center", va="center", fontsize=6, color="white")

    y_ticks = list(y_map.values())
    y_labels = [f"I{inn}.O{over + 1}" for inn, over in rows]
    ax.set_yticks(y_ticks)
    ax.set_yticklabels(y_labels, fontsize=7)
    ax.set_xlabel("Simulation Time (seconds)", fontsize=9)
    ax.set_title("Bowler Critical-Section Gantt by Innings", fontsize=11, fontweight="bold")
    ax.grid(axis="x", linestyle="--", alpha=0.35)
    ax.invert_yaxis()

    handles = [mpatches.Patch(color=colour, label=bowler) for bowler, colour in palette.items()]
    if handles:
        ax.legend(handles=handles, fontsize=7, loc="upper right", ncol=2)


def plot_thread_timeline(df: pd.DataFrame, ax: plt.Axes) -> None:
    row_map = {
        "BALL_BOWLED": 0,
        "WIDE": 0,
        "NO_BALL": 0,
        "FREE_HIT": 0,
        "BALL_IN_AIR": 1,
        "RUNS_SCORED": 1,
        "FOUR": 1,
        "SIX": 1,
        "BATSMAN_OUT": 2,
        "OVER_COMPLETE": 3,
        "MATCH_OVER": 3,
    }
    row_labels = {
        0: "Bowler",
        1: "Bat/Fielder",
        2: "Umpire",
        3: "Scheduler/End",
    }
    markers = {1: "o", 2: "s", 3: "^", 4: "D"}

    for evt, row in row_map.items():
        subset = df[df["event_type"] == evt]
        if subset.empty:
            continue
        for innings_no, inning_df in subset.groupby("innings", sort=True):
            ax.scatter(
                inning_df["t_sec"],
                [row] * len(inning_df),
                color=EVENT_COLOURS.get(evt, DEFAULT_COLOUR),
                marker=markers.get(int(innings_no), "o"),
                s=18,
                alpha=0.75,
                label=f"{evt}:{innings_no}",
            )

    for innings_no in sorted(df["innings"].unique()):
        inning_df = df[df["innings"] == innings_no]
        if inning_df.empty:
            continue
        ax.axvspan(inning_df["t_sec"].min(), inning_df["t_sec"].max(), alpha=0.03, color="#34495e")
        ax.text(inning_df["t_sec"].min(), 3.25, f"Innings {innings_no}", fontsize=8, fontweight="bold")

    ax.set_yticks(list(row_labels.keys()))
    ax.set_yticklabels(list(row_labels.values()), fontsize=8)
    ax.set_xlabel("Simulation Time (seconds)", fontsize=9)
    ax.set_title("Thread Event Timeline Across Full Match", fontsize=11, fontweight="bold")
    ax.grid(axis="x", linestyle="--", alpha=0.35)


def plot_run_rate(df: pd.DataFrame, ax: plt.Axes) -> None:
    positions = []
    runs = []
    labels = []
    colours = []
    wicket_points = []
    gap = 1
    x_cursor = 0

    for innings_no, inning_df in df.groupby("innings", sort=True):
        innings_runs = inning_df[inning_df["event_type"].isin(["RUNS_SCORED", "FOUR", "SIX", "WIDE", "NO_BALL"])]
        max_over = int(inning_df["over"].max()) if not inning_df.empty else -1
        for over in range(max_over + 1):
            over_df = innings_runs[innings_runs["over"] == over]
            positions.append(x_cursor)
            runs.append(int(over_df["runs_off_ball"].sum()) if not over_df.empty else 0)
            labels.append(f"I{innings_no}.{over + 1}")
            phase = (
                inning_df[inning_df["over"] == over]["match_phase"].iloc[0]
                if not inning_df[inning_df["over"] == over].empty
                else ""
            )
            colours.append(PHASE_COLOURS.get(phase, DEFAULT_COLOUR))

            wkts = inning_df[(inning_df["over"] == over) & (inning_df["event_type"] == "BATSMAN_OUT")]
            wicket_points.extend([(x_cursor, len(wkts))] if not wkts.empty else [])
            x_cursor += 1
        x_cursor += gap

    ax.bar(positions, runs, color=colours, alpha=0.8, edgecolor="white", width=0.8)
    for x, wicket_count in wicket_points:
        ax.text(x, runs[positions.index(x)] + 0.4, f"{wicket_count}W", ha="center", fontsize=7, color="#c0392b")

    ax.set_xticks(positions)
    ax.set_xticklabels(labels, rotation=70, fontsize=7)
    ax.set_xlabel("Over by Innings", fontsize=9)
    ax.set_ylabel("Runs", fontsize=9)
    ax.set_title("Runs per Over Split by Innings", fontsize=11, fontweight="bold")
    ax.grid(axis="y", linestyle="--", alpha=0.35)

    handles = [mpatches.Patch(color=colour, label=phase) for phase, colour in PHASE_COLOURS.items()]
    ax.legend(handles=handles, fontsize=7, loc="upper left")


def plot_scheduler(df: pd.DataFrame, ax: plt.Axes, palette: dict) -> None:
    oc = df[df["event_type"] == "OVER_COMPLETE"].copy()
    if oc.empty:
        ax.text(0.5, 0.5, "No OVER_COMPLETE events found", ha="center", va="center", transform=ax.transAxes)
        return

    bowlers = sorted([name for name in df["bowler_name"].unique().tolist() if name])
    bowler_idx = {name: i for i, name in enumerate(bowlers)}

    xs = []
    ys = []
    cs = []
    for innings_no, inning_df in df[df["event_type"] == "BALL_BOWLED"].groupby("innings", sort=True):
        for over in sorted(inning_df["over"].unique()):
            over_df = inning_df[inning_df["over"] == over]
            if over_df.empty:
                continue
            bowler = over_df["bowler_name"].mode().iloc[0]
            xs.append(over_df["t_sec"].min())
            ys.append(bowler_idx.get(bowler, 0))
            cs.append(palette.get(bowler, DEFAULT_COLOUR))

    ax.step(xs, ys, where="post", color="#2c3e50", linewidth=1.2, alpha=0.6)
    ax.scatter(xs, ys, color=cs, s=50, zorder=5)
    ax.set_yticks(list(bowler_idx.values()))
    ax.set_yticklabels(list(bowler_idx.keys()), fontsize=8)
    ax.set_xlabel("Simulation Time (seconds)", fontsize=9)
    ax.set_title("Scheduler Context Switch Trace", fontsize=11, fontweight="bold")
    ax.grid(axis="x", linestyle="--", alpha=0.35)


def add_summary_box(fig: plt.Figure, df: pd.DataFrame, mode_label: str) -> None:
    summary_lines = [f"Dataset: {mode_label}", f"Total events: {len(df)}"]
    for item in innings_summaries(df):
        summary_lines.append(
            f"I{item['innings']}: {item['score']} in {item['overs']} overs"
        )
    fig.text(
        0.012,
        0.985,
        "\n".join(summary_lines),
        ha="left",
        va="top",
        fontsize=8,
        bbox=dict(boxstyle="round,pad=0.35", facecolor="#f4f6f7", edgecolor="#d0d3d4"),
    )


def make_gantt_figure(df: pd.DataFrame, out_path: str, label: str) -> None:
    palette = build_bowler_palette(df)
    fig, axes = plt.subplots(4, 1, figsize=(18, 22))
    fig.suptitle(
        "T20WC Cricket Simulator — Full Match Thread & Resource Visualisation\n"
        f"Dataset: {label}",
        fontsize=13,
        fontweight="bold",
        y=0.995,
    )

    plot_bowler_gantt(df, axes[0], palette)
    plot_thread_timeline(df, axes[1])
    plot_run_rate(df, axes[2])
    plot_scheduler(df, axes[3], palette)
    add_summary_box(fig, df, label)

    plt.tight_layout(rect=(0.03, 0.03, 0.98, 0.96))
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"[gantt_plot] Saved {label} -> {out_path}")


def resolve_datasets(args) -> list[tuple[str, str]]:
    if args.csv:
        return [(args.label or Path(args.csv).stem, args.csv)]

    datasets = []
    for mode, path in MODE_DEFAULTS.items():
        selected = getattr(args, f"csv_{mode}")
        if selected:
            datasets.append((mode.upper(), selected))
    return datasets


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate Gantt/timeline charts from simulator logs")
    parser.add_argument("--csv", default=None, help="Single CSV input")
    parser.add_argument("--label", default=None, help="Label for --csv mode")
    parser.add_argument("--out", default=None, help="Output PNG path for --csv mode")
    parser.add_argument("--csv-fcfs", default=MODE_DEFAULTS["fcfs"], help="FCFS CSV path")
    parser.add_argument("--csv-sjf", default=MODE_DEFAULTS["sjf"], help="SJF CSV path")
    parser.add_argument("--csv-priority", default=MODE_DEFAULTS["priority"], help="Priority CSV path")
    parser.add_argument("--out-prefix", default="docs/gantt", help="Output path prefix for batch mode")
    args = parser.parse_args()

    datasets = resolve_datasets(args)
    if not datasets:
        print("ERROR: No datasets selected.")
        sys.exit(1)

    if args.csv:
        out_path = args.out or "docs/gantt.png"
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        df = load_events(args.csv)
        make_gantt_figure(df, out_path, datasets[0][0])
        return

    os.makedirs(os.path.dirname(args.out_prefix) or ".", exist_ok=True)
    for label, csv_path in datasets:
        if not os.path.exists(csv_path):
            print(f"[gantt_plot] Skipping {label}: {csv_path} not found")
            continue
        out_path = f"{args.out_prefix}_{label.lower()}.png"
        df = load_events(csv_path)
        make_gantt_figure(df, out_path, label)


if __name__ == "__main__":
    main()
