#!/usr/bin/env python3
# 性能基准汇总：读一份运行日志，取回放期间（B3-REPLAY 到 B3-REPLAY-END）每 300 帧一行的“计时”“帧间隔”记录，以及长帧、热状态，打印一段摘要。
# 只用标准库。用法：bench_summary.py <日志> [标签]
# Benchmark summary: reads one run log, takes the per-300-frame "timing" / "frame interval" lines during the replay (B3-REPLAY to B3-REPLAY-END), plus long frames and thermal state, and prints a summary.
# Standard library only. Usage: bench_summary.py <log> [label]
import re, statistics, sys

def main():
    path = sys.argv[1]; label = sys.argv[2] if len(sys.argv) > 2 else path
    on, rows, over, total, longf, thermal = False, [], 0, 0, [], []
    for l in open(path, errors='replace'):
        if 'B3-REPLAY 回放输入' in l: on = True
        if 'B3-REPLAY-END' in l: on = False
        m = re.search(r'B3-THERMAL (\w+)', l)
        if m: thermal.append(m.group(1))
        if not on: continue
        m = re.search(r'计时 至帧 \d+： 帧时间 p50 ([\d.]+) / p95 ([\d.]+) / 最大 ([\d.]+) ms.*GPU p50 ([\d.]+) / p95 ([\d.]+) / 最大 ([\d.]+) ms', l)
        if m: rows.append([float(x) for x in m.groups()])
        m = re.search(r'帧间隔 至帧 \d+：>16.7ms (\d+) / (\d+) 帧', l)
        if m: over += int(m.group(1)); total += int(m.group(2))
        m = re.search(r'\[长帧\] 帧 \d+（[\d.]+ s）整帧 ([\d.]+) ms', l)
        if m: longf.append(float(m.group(1)))
    if not rows: sys.exit('日志里没有回放期间的计时记录 | no timing lines during replay in the log')
    med = lambda i: statistics.median(r[i] for r in rows)
    print(f'[{label}] {len(rows) * 300} 帧 | frames')
    print(f'  帧时间 frame time  p50 {med(0):.1f} ms（约 {1000 / med(0):.0f} fps）  p95 {statistics.median(r[1] for r in rows):.1f} ms  最差窗口 p95 worst {max(r[1] for r in rows):.1f} ms  最大 max {max(r[2] for r in rows):.0f} ms')
    print(f'  GPU               p50 {med(3):.1f} ms  p95 {statistics.median(r[4] for r in rows):.1f} ms  最差 worst {max(r[5] for r in rows):.1f} ms')
    print(f'  卡顿 stalls ≥ 100 ms  {sum(x >= 100 for x in longf)} 次，最长 longest {max(longf, default=0):.0f} ms')
    if thermal: print(f'  热状态 thermal  {" → ".join(dict.fromkeys(thermal))}')

if __name__ == '__main__':
    main()
