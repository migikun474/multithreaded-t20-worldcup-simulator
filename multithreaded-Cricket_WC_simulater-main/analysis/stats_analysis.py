#!/usr/bin/env python3
"""
stats_analysis.py — Full-match statistical analysis for simulator CSV logs.
Handles one or two innings per file and batch-processes FCFS/SJF/Priority outputs.
"""

import argparse
import json
import os
import sys
from pathlib import Path

try:
    import numpy as np
    import pandas as pd
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    print("ERROR: pip install matplotlib pandas numpy")
    sys.exit(1)


MODE_DEFAULTS = {
    "fcfs": "../logs/events_fcfs.csv",
    "sjf": "../logs/events_sjf.csv",
    "priority": "../logs/events_priority.csv",
}
PHASE_COLOURS = {
    "Powerplay": "#27ae60",
    "Middle": "#2980b9",
    "Middle-late": "#8e44ad",
    "Death": "#c0392b",
}
DISMISS_COLOURS = {
    "stumped": "#e74c3c",
    "lbw": "#3498db",
    "caught": "#f39c12",
    "run out": "#9b59b6",
    "run_out": "#9b59b6",
    "bowled": "#2ecc71",
    "other": "#95a5a6",
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


def compute_wait_times(inning_df: pd.DataFrame) -> dict:
    bowled = inning_df[inning_df["event_type"] == "BALL_BOWLED"].copy()
    if bowled.empty:
        return {}
    first_seen = bowled.groupby("batsman_name")["t_sec"].min().sort_values()
    t0 = float(inning_df["t_sec"].min()) if not inning_df.empty else 0.0
    return {name: max(0.0, float(ts - t0)) for name, ts in first_seen.items() if name}


def compute_summary(df: pd.DataFrame, label: str) -> dict:
    innings = []
    for innings_no, inning_df in df.groupby("innings", sort=True):
        balls = len(inning_df[inning_df["event_type"] == "BALL_BOWLED"])
        innings.append(
            {
                "innings": int(innings_no),
                "runs": int(inning_df["total_runs"].max()) if not inning_df.empty else 0,
                "wickets": int(inning_df["total_wickets"].max()) if not inning_df.empty else 0,
                "balls": balls,
                "overs": f"{balls // 6}.{balls % 6}",
                "fours": int((inning_df["event_type"] == "FOUR").sum()),
                "sixes": int((inning_df["event_type"] == "SIX").sum()),
                "wides": int((inning_df["event_type"] == "WIDE").sum()),
                "no_balls": int((inning_df["event_type"] == "NO_BALL").sum()),
                "free_hits": int((inning_df["event_type"] == "FREE_HIT").sum()),
                "wait_times_sec": compute_wait_times(inning_df),
            }
        )

    algo = "Unknown"
    oc = df[df["event_type"] == "OVER_COMPLETE"]
    if not oc.empty:
        msg = oc["message"].iloc[0]
        if "Shortest Job" in msg:
            algo = "SJF"
        elif "Priority" in msg:
            algo = "Priority"
        elif "Round Robin" in msg:
            algo = "FCFS"

    return {
        "dataset": label,
        "scheduler_algo": algo,
        "total_events": int(len(df)),
        "innings_count": int(df["innings"].nunique()),
        "innings": innings,
    }


def print_summary(summary: dict) -> None:
    print("\n" + "=" * 64)
    print(f"DATASET: {summary['dataset']} | ALGO: {summary['scheduler_algo']}")
    print("=" * 64)
    for inning in summary["innings"]:
        print(
            f"Innings {inning['innings']}: {inning['runs']}/{inning['wickets']} "
            f"in {inning['overs']} overs | 4s={inning['fours']} 6s={inning['sixes']} "
            f"Wd={inning['wides']} Nb={inning['no_balls']} FH={inning['free_hits']}"
        )
    print("=" * 64 + "\n")


def plot_wait_times(df: pd.DataFrame, ax: plt.Axes) -> None:
    innings = sorted(df["innings"].unique())
    all_labels = []
    all_values = []
    colours = []

    for innings_no in innings:
        waits = compute_wait_times(df[df["innings"] == innings_no])
        for batsman, value in waits.items():
            all_labels.append(f"I{innings_no}\n{batsman}")
            all_values.append(value)
            colours.append("#2ecc71" if innings_no == 1 else "#e67e22")

    if not all_values:
        ax.text(0.5, 0.5, "No batsman wait-time data", ha="center", va="center", transform=ax.transAxes)
        return

    x = np.arange(len(all_labels))
    ax.bar(x, all_values, color=colours, alpha=0.82)
    ax.set_xticks(x)
    ax.set_xticklabels(all_labels, fontsize=7)
    ax.set_ylabel("Seconds from innings start", fontsize=9)
    ax.set_title("Batsman Arrival Wait Times by Innings", fontsize=10, fontweight="bold")
    ax.grid(axis="y", linestyle="--", alpha=0.35)


def plot_bowler_utilisation(df: pd.DataFrame, ax: plt.Axes) -> None:
    bowled = df[df["event_type"] == "BALL_BOWLED"].copy()
    if bowled.empty:
        ax.text(0.5, 0.5, "No BALL_BOWLED events", ha="center", va="center", transform=ax.transAxes)
        return

    bowlers = sorted([name for name in bowled["bowler_name"].unique().tolist() if name])
    x = np.arange(len(bowlers))
    width = 0.38

    for idx, innings_no in enumerate(sorted(df["innings"].unique())):
        inning_df = bowled[bowled["innings"] == innings_no]
        counts = [len(inning_df[inning_df["bowler_name"] == bowler]) for bowler in bowlers]
        ax.bar(
            x + (idx - 0.5) * width,
            counts,
            width,
            label=f"Innings {innings_no}",
            alpha=0.82,
        )

    ax.set_xticks(x)
    ax.set_xticklabels(bowlers, rotation=25, ha="right", fontsize=8)
    ax.set_ylabel("Legal deliveries", fontsize=9)
    ax.set_title("Bowler Utilisation by Innings", fontsize=10, fontweight="bold")
    ax.legend(fontsize=8)
    ax.grid(axis="y", linestyle="--", alpha=0.35)


def plot_run_rate_trend(df: pd.DataFrame, ax: plt.Axes) -> None:
    x_pos = []
    labels = []
    values = []
    colours = []
    cursor = 0

    scoring = df[df["event_type"].isin(["RUNS_SCORED", "FOUR", "SIX", "WIDE", "NO_BALL"])]
    for innings_no, inning_df in df.groupby("innings", sort=True):
        max_over = int(inning_df["over"].max()) if not inning_df.empty else -1
        for over in range(max_over + 1):
            over_df = scoring[(scoring["innings"] == innings_no) & (scoring["over"] == over)]
            values.append(int(over_df["runs_off_ball"].sum()) if not over_df.empty else 0)
            x_pos.append(cursor)
            labels.append(f"I{innings_no}.{over + 1}")
            phase = (
                inning_df[inning_df["over"] == over]["match_phase"].iloc[0]
                if not inning_df[inning_df["over"] == over].empty
                else ""
            )
            colours.append(PHASE_COLOURS.get(phase, "#95a5a6"))
            cursor += 1
        cursor += 1

    ax.bar(x_pos, values, color=colours, alpha=0.8)
    ma = pd.Series(values).rolling(3, min_periods=1).mean()
    ax.plot(x_pos, ma, color="#2c3e50", linewidth=1.8, marker="o", markersize=3)
    ax.set_xticks(x_pos)
    ax.set_xticklabels(labels, rotation=70, fontsize=7)
    ax.set_ylabel("Runs", fontsize=9)
    ax.set_title("Runs per Over Across Both Innings", fontsize=10, fontweight="bold")
    ax.grid(axis="y", linestyle="--", alpha=0.35)


def plot_dismissal_breakdown(df: pd.DataFrame, ax: plt.Axes) -> None:
    wickets = df[df["event_type"] == "BATSMAN_OUT"].copy()
    if wickets.empty:
        ax.text(0.5, 0.5, "No wickets recorded", ha="center", va="center", transform=ax.transAxes)
        return

    wickets["dismissal_norm"] = wickets["dismissal_type"].replace("", "other")
    counts = wickets.groupby(["innings", "dismissal_norm"]).size().unstack(fill_value=0)
    counts = counts.reindex(sorted(counts.index))

    bottom = np.zeros(len(counts.index))
    for dismissal in counts.columns:
        ax.bar(
            counts.index.astype(str),
            counts[dismissal].values,
            bottom=bottom,
            label=dismissal,
            color=DISMISS_COLOURS.get(dismissal, DISMISS_COLOURS["other"]),
            alpha=0.85,
        )
        bottom += counts[dismissal].values

    ax.set_xlabel("Innings", fontsize=9)
    ax.set_ylabel("Wickets", fontsize=9)
    ax.set_title("Dismissal Type Breakdown by Innings", fontsize=10, fontweight="bold")
    ax.legend(fontsize=7)
    ax.grid(axis="y", linestyle="--", alpha=0.35)


def plot_thread_event_rate(df: pd.DataFrame, ax: plt.Axes) -> None:
    event_groups = {
        "Bowler": ["BALL_BOWLED", "WIDE", "NO_BALL", "FREE_HIT"],
        "Bat/Fielder": ["BALL_IN_AIR", "RUNS_SCORED", "FOUR", "SIX"],
        "Umpire": ["BATSMAN_OUT"],
        "Scheduler": ["OVER_COMPLETE", "MATCH_OVER"],
    }
    t_max = float(df["t_sec"].max()) if not df.empty else 0.0
    bins = np.arange(0, t_max + 0.5, 0.5)
    if len(bins) < 2:
        bins = np.array([0.0, 0.5])
    centers = (bins[:-1] + bins[1:]) / 2

    for label, events in event_groups.items():
        subset = df[df["event_type"].isin(events)]
        counts, _ = np.histogram(subset["t_sec"].values, bins=bins)
        ax.plot(centers, pd.Series(counts).rolling(3, min_periods=1).mean(), label=label, linewidth=1.6)

    ax.set_xlabel("Simulation Time (seconds)", fontsize=9)
    ax.set_ylabel("Events / 0.5s", fontsize=9)
    ax.set_title("Thread Activity Rate Across Full Match", fontsize=10, fontweight="bold")
    ax.legend(fontsize=8)
    ax.grid(linestyle="--", alpha=0.35)


def plot_crease_occupancy(df: pd.DataFrame, ax: plt.Axes) -> None:
    timeline = df[df["event_type"].isin(["BALL_BOWLED", "BATSMAN_OUT"])].copy().sort_values("t_sec")
    if timeline.empty:
        ax.text(0.5, 0.5, "No crease occupancy data", ha="center", va="center", transform=ax.transAxes)
        return

    times = []
    occ = []
    current = 2
    current_innings = int(timeline["innings"].iloc[0])
    times.append(float(timeline["t_sec"].iloc[0]))
    occ.append(2)

    for _, row in timeline.iterrows():
        if int(row["innings"]) != current_innings:
            current_innings = int(row["innings"])
            current = 2
            times.append(float(row["t_sec"]))
            occ.append(current)
        elif row["event_type"] == "BATSMAN_OUT":
            current = max(1, current - 1)
            times.append(float(row["t_sec"]))
            occ.append(current)
        elif row["event_type"] == "BALL_BOWLED" and current < 2:
            current = 2
            times.append(float(row["t_sec"]))
            occ.append(current)

    ax.step(times, occ, where="post", linewidth=1.8, color="#2980b9")
    ax.fill_between(times, occ, step="post", alpha=0.2, color="#3498db")
    ax.set_ylim(0.8, 2.2)
    ax.set_yticks([1, 2])
    ax.set_yticklabels(["1", "2"], fontsize=8)
    ax.set_xlabel("Simulation Time (seconds)", fontsize=9)
    ax.set_ylabel("Batsmen at crease", fontsize=9)
    ax.set_title("Crease Semaphore Occupancy", fontsize=10, fontweight="bold")
    ax.grid(linestyle="--", alpha=0.35)


def plot_extras(df: pd.DataFrame, ax: plt.Axes) -> None:
    extra_types = ["WIDE", "NO_BALL", "FREE_HIT"]
    rows = []
    for innings_no, inning_df in df.groupby("innings", sort=True):
        rows.append(
            [
                innings_no,
                int((inning_df["event_type"] == "WIDE").sum()),
                int((inning_df["event_type"] == "NO_BALL").sum()),
                int((inning_df["event_type"] == "FREE_HIT").sum()),
            ]
        )
    if not rows:
        ax.text(0.5, 0.5, "No extras data", ha="center", va="center", transform=ax.transAxes)
        return

    extras_df = pd.DataFrame(rows, columns=["innings", "WIDE", "NO_BALL", "FREE_HIT"])
    x = np.arange(len(extras_df))
    bottom = np.zeros(len(extras_df))
    colours = {"WIDE": "#e67e22", "NO_BALL": "#e74c3c", "FREE_HIT": "#9b59b6"}

    for evt in extra_types:
        ax.bar(x, extras_df[evt], bottom=bottom, label=evt, color=colours[evt], alpha=0.85)
        bottom += extras_df[evt].values

    ax.set_xticks(x)
    ax.set_xticklabels([f"Innings {i}" for i in extras_df["innings"]], fontsize=8)
    ax.set_ylabel("Count", fontsize=9)
    ax.set_title("Extras by Innings", fontsize=10, fontweight="bold")
    ax.legend(fontsize=8)
    ax.grid(axis="y", linestyle="--", alpha=0.35)


def make_figure(df: pd.DataFrame, summary: dict, out_path: str) -> None:
    fig, axes = plt.subplots(4, 2, figsize=(20, 28))
    fig.suptitle(
        "T20WC Cricket Simulator — Full Match Statistical Analysis\n"
        f"Dataset: {summary['dataset']} | Algorithm: {summary['scheduler_algo']}",
        fontsize=13,
        fontweight="bold",
        y=0.995,
    )

    plot_wait_times(df, axes[0, 0])
    plot_bowler_utilisation(df, axes[0, 1])
    plot_run_rate_trend(df, axes[1, 0])
    plot_dismissal_breakdown(df, axes[1, 1])
    plot_thread_event_rate(df, axes[2, 0])
    plot_crease_occupancy(df, axes[2, 1])
    plot_extras(df, axes[3, 0])

    axes[3, 1].axis("off")
    text_lines = [f"Total events: {summary['total_events']}"]
    for inning in summary["innings"]:
        text_lines.append(
            f"I{inning['innings']}: {inning['runs']}/{inning['wickets']} in {inning['overs']} overs"
        )
    axes[3, 1].text(
        0.02,
        0.98,
        "\n".join(text_lines),
        ha="left",
        va="top",
        fontsize=11,
        bbox=dict(boxstyle="round,pad=0.4", facecolor="#f4f6f7", edgecolor="#d0d3d4"),
    )

    plt.tight_layout(rect=(0.03, 0.03, 0.98, 0.97))
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"[stats_analysis] Saved -> {out_path}")


def resolve_datasets(args) -> list[tuple[str, str]]:
    if args.csv:
        return [(args.label or Path(args.csv).stem, args.csv)]
    return [
        ("FCFS", args.csv_fcfs),
        ("SJF", args.csv_sjf),
        ("PRIORITY", args.csv_priority),
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate full-match statistical analysis figures")
    parser.add_argument("--csv", default=None, help="Single CSV input")
    parser.add_argument("--label", default=None, help="Dataset label for --csv mode")
    parser.add_argument("--out", default=None, help="Single output PNG path")
    parser.add_argument("--json", default=None, help="Single output JSON path")
    parser.add_argument("--csv-fcfs", default=MODE_DEFAULTS["fcfs"], help="FCFS CSV path")
    parser.add_argument("--csv-sjf", default=MODE_DEFAULTS["sjf"], help="SJF CSV path")
    parser.add_argument("--csv-priority", default=MODE_DEFAULTS["priority"], help="Priority CSV path")
    parser.add_argument("--out-prefix", default="docs/analysis", help="Batch output PNG prefix")
    parser.add_argument("--json-prefix", default="docs/summary", help="Batch output JSON prefix")
    args = parser.parse_args()

    datasets = resolve_datasets(args)
    if args.csv:
        out_path = args.out or "docs/analysis.png"
        json_path = args.json or "docs/summary.json"
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        os.makedirs(os.path.dirname(json_path) or ".", exist_ok=True)
        df = load_events(args.csv)
        summary = compute_summary(df, datasets[0][0])
        print_summary(summary)
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(summary, fh, indent=2)
        make_figure(df, summary, out_path)
        return

    os.makedirs(os.path.dirname(args.out_prefix) or ".", exist_ok=True)
    os.makedirs(os.path.dirname(args.json_prefix) or ".", exist_ok=True)

    for label, csv_path in datasets:
        if not os.path.exists(csv_path):
            print(f"[stats_analysis] Skipping {label}: {csv_path} not found")
            continue
        df = load_events(csv_path)
        summary = compute_summary(df, label)
        print_summary(summary)
        out_path = f"{args.out_prefix}_{label.lower()}.png"
        json_path = f"{args.json_prefix}_{label.lower()}.json"
        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(summary, fh, indent=2)
        make_figure(df, summary, out_path)


if __name__ == "__main__":
    main()
