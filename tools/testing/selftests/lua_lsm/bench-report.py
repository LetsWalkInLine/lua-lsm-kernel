#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Summarize formal Lua-LSM benchmark runs into report tables.

Reads the summary.json produced by bench.py stats plus the per-boot
parsed.json files, and prints markdown tables for the three
measurements: syscall overhead with kernel increments, string-quota
corpus cost, and VM deadline expiry latency.  Medians of per-boot
means accompany pooled means so whole-boot host-frequency outliers
stay visible instead of silently shifting averages.
"""

import argparse
import json
import math
import statistics
from pathlib import Path


def fmt_us(v):
    return f"{v / 1000:.2f}"


def fmt_ratio(v):
    return f"{v:.3f}" if v is not None else "-"


def boot_med(series):
    return statistics.median(series) if series else None


def load(runs):
    runs = Path(runs)
    summary = json.loads((runs / "summary.json").read_text())
    boots = []
    for d in sorted(runs.glob("boot-*")):
        pj, ij = d / "parsed.json", d / "identity.json"
        if pj.exists() and ij.exists():
            boots.append(json.loads(pj.read_text()))
    return summary, boots


def sys_tables(summary, boots):
    lines = ["## 正常系统调用开销（openat，单次样本pooled）", "",
             "| 场景 | 指标 | B0 | B1 | B2 | B1/B0 | B2/B1 | B2/B0 |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    scenarios = sorted({k.split("/", 1)[1] for k in summary["sys"]})
    for scn in scenarios:
        entries = {k.split("/")[0]: summary["sys"][k]
                   for k in summary["sys"] if k.split("/", 1)[1] == scn}
        for metric, label in (("mean", "mean ns"), ("p95", "p95 ns"), ("p99", "p99 ns")):
            vals = {k: e["samples"]["pooled"][metric] for k, e in entries.items()}
            if not vals:
                continue
            row = [f"| {scn} | {label} |"]
            for k in ("b0", "b1", "b2"):
                row.append(f" {vals[k]:.0f} |" if k in vals else " - |")
            for a, b in (("b0", "b1"), ("b1", "b2"), ("b0", "b2")):
                row.append(f" {fmt_ratio(vals[b] / vals[a])} |" if a in vals and b in vals else " - |")
            lines.append("".join(row))
        # throughput
        vals = {k: e["throughput"]["pooled"]["mean"] for k, e in entries.items()}
        row = [f"| {scn} | 吞吐 ns/op |"]
        for k in ("b0", "b1", "b2"):
            row.append(f" {vals[k]:.0f} |" if k in vals else " - |")
        lines.append("".join(row))
        lines.append("")
    inc_lines = ["### 增量（pooled mean，ns）", "",
                 "| 场景 | B1-B0 | B2-B1 | B2-B0 |", "| --- | ---: | ---: | ---: |"]
    for scn in scenarios:
        cells = []
        for pair in ("b1-b0", "b2-b1", "b2-b0"):
            e = summary["sys_increments"].get(f"{scn}:{pair}")
            cells.append(f" {e['delta_boot_mean_ns']:+.0f} |" if e else " - |")
        inc_lines.append(f"| {scn} |" + "".join(cells))
    return lines, inc_lines


def corpus_tables(summary):
    items = ["allow", "lower4096", "labelmatch65536", "replace65536",
             "findnear76190", "escapenear76190", "escapeover76191",
             "findover76191", "trimsp1026", "basename4095"]
    lines = ["## 字符串额度对应的内核耗时（warm单次ktime，μs）", "",
             "| 语料 | 类别(B0) | 类别(B1) | 类别(B2) | B0 mean | B1 mean | B2 mean | B1/B0 | B2/B1 | B2/B0 |",
             "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for it in items:
        cats, means = {}, {}
        for k in ("b0", "b1", "b2"):
            e = summary["kern_corpus"].get(f"{k}/{it}/raw")
            if e:
                cats[k] = {0: "成功", -5: "EIO", -16: "-"}[e["cat"]]
                means[k] = e["warm"]["mean"]
        if not means:
            continue
        row = [f"| {it} "]
        row.append(f"| {cats.get('b0','-')} | {cats.get('b1','-')} | {cats.get('b2','-')} |")
        for k in ("b0", "b1", "b2"):
            row.append(f" {fmt_us(means[k])} |" if k in means else " - |")
        for a, b in (("b0", "b1"), ("b1", "b2"), ("b0", "b2")):
            row.append(f" {fmt_ratio(means[b] / means[a])} |" if a in means and b in means else " - |")
        lines.append("".join(row))
    lines += ["", "类别：成功=未捕获成功；EIO=超限未捕获错误；-=该内核上caught变体的类别（work错误按errno 16拒绝）", ""]
    return lines


def expiry_tables(summary):
    lines = ["## VM预算到期后的结束延迟（总时长−预算−入口基线，ns）", "",
             "| 路径 | 上下文 | 预算ms | total p50 | total p99 | total max | 结束延迟均值 | 结束延迟p99 |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for key, e in sorted(summary["kern_expiry"].items()):
        if not e.get("totals"):
            continue
        shape, ctx, kind = key.split("/")
        if kind != "exp_timeout":
            continue
        t = e["totals"]
        budget = e["budget"] or 0
        am = [a for a in (e.get("allow_mean_per_boot") or []) if a]
        am = statistics.median(am) if am else 0
        lines.append(f"| {shape} | {ctx} | {budget / 1e6:.0f} | {t['p50']:.0f} | "
                     f"{t['p99']:.0f} | {t['max']:.0f} | {t['mean'] - budget - am:.0f} | "
                     f"{t['p99'] - budget - am:.0f} |")
    lines.append("")
    rec = [f"| {k.split('/')[0]}/{k.split('/')[1]} | 恢复调用 mean |" for k in summary["kern_expiry"] if k.endswith("exp_recovery")]
    lines += ["### 超时后同VM恢复调用（allow策略，warm）", "",
              "| 路径/上下文 | mean ns | p99 ns |", "| --- | ---: | ---: |"]
    for key, e in sorted(summary["kern_expiry"].items()):
        if not key.endswith("exp_recovery"):
            continue
        t = e["totals"]
        shape, ctx = key.split("/")[:2]
        lines.append(f"| {shape}/{ctx} | {t['mean']:.0f} | {t['p99']:.0f} |")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--runs", required=True)
    args = ap.parse_args()
    summary, boots = load(args.runs)
    sysl, inc = sys_tables(summary, boots)
    print("\n".join(sysl))
    print("\n".join(inc))
    corpus = corpus_tables(summary)
    print("\n".join(corpus))
    expiry = expiry_tables(summary)
    print("\n".join(expiry))


if __name__ == "__main__":
    main()
