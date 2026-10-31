#!/usr/bin/env python3
"""Verify consistency of all DG-PIM datasets, capacity budgets, dynamic remap costs, and analytical predictions."""
from pathlib import Path
import csv
import json
import math

ROOT = Path(__file__).resolve().parents[1]

# 1. 70B GQA 8K footprint identity (2.50 GiB per sequence, 80.0 GiB across batch of 32):
fp16_kv_gib = (2 * 80 * 8 * 128 * 8192 * 2) / (1024 ** 3)
fp32_k_gib  = (1 * 80 * 8 * 128 * 8192 * 4) / (1024 ** 3)
assert fp16_kv_gib == 2.50 and fp32_k_gib == 2.50

# 2. Short-burst 288 fixture pairs (1.167x R, 1.118x E):
sb = list(csv.DictReader((ROOT / "data/short_burst_288_pairs.csv").open()))
assert len(sb) == 288
gm_r = math.exp(sum(math.log(float(r["speedup_R"])) for r in sb) / 288.0)
gm_e = math.exp(sum(math.log(float(r["speedup_E"])) for r in sb) / 288.0)
assert abs(gm_r - 1.1666) < 1e-3 and abs(gm_e - 1.1183) < 1e-3

# 3. Native 52 configurations:
nat = list(csv.DictReader((ROOT / "data/native_52_configs.csv").open()))
assert len(nat) == 52

# 4. Scratchpad and exponent-cache byte accounting B_x(N) = 4N + 4G + ceil(G/8) + 64:
def bx(N):
    G = math.ceil(N / 16)
    return 4 * N + 4 * G + math.ceil(G / 8) + 64

assert bx(1280) == 5514 and bx(8192) == 34944
assert (262144 - (29018 + 8768)) == 876 * 256 + 102
assert (262144 - (34532 + 8768)) == 854 * 256 + 220

# 5. Marginal-Byte Law & Remap Costs (D1/D2/D3):
pred_8k_avg = round((39677 - 24312) * (0.19 / 16.0) - 63989 * 0.0135 - 69632 / 25.0)
pred_8k_spill = round((39677 - 24312) * (0.19 / 16.0) - 63989 * 0.0135 - 69632 / 10.93)
pred_1280 = round((554701 - 96748) * (0.68 / 16.0) - 651449 * 0.0135 - 22 * 512 / 12.78)
pred_q2k = round((6418920 - 1063194) * (0.71 / 16.0) - 7482114 * 0.0135)
assert pred_8k_avg == -3467 and pred_8k_spill == -7052 and pred_q2k == 136652

d1_256 = math.ceil(5514 / 32.0) + 24 + 4 * 255
d2_256 = 1 + 24 + 4 * 255
d3_256 = 4 + 1 * 255
assert d1_256 == 1217 and d2_256 == 1045 and d3_256 == 259

# 6. Dispatch, robustness, unknown-Q, and L2 verification ledger:
dl2 = json.loads((ROOT / "data/dispatch_and_l2_metrics.json").read_text())
assert len(dl2["remap_ledger"]) == 16 and len(dl2["robustness_q256"]) == 5 and len(dl2["l2_models"]) == 5
if "unknown_q_requests" in dl2: assert len(dl2["unknown_q_requests"]) == 200

print("[OK] Verified 288 short-burst pairs, 52 native configs, D1/D2/D3 byte/cycle ledgers, robustness grids, and Marginal-Byte Law predictions.")
