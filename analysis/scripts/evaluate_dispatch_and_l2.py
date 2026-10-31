#!/usr/bin/env python3
"""Generate deterministic analytical scenarios for memory allocation and system projections.

This utility evaluates a fixed parameter table. It does not execute native
PIM simulation or validate independent hardware measurements.
"""
from pathlib import Path
import json
import math
import random
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "data"

random.seed(202705)
np.random.seed(202705)


def remap_costs(bx_bytes: float, q_steps: int):
    if q_steps <= 0:
        return 0, 0, 0
    d1_scrub = math.ceil(bx_bytes / 32.0) + 24 + 4 * (q_steps - 1)
    d2_bitmap = 1 + 24 + 4 * (q_steps - 1)
    d3_epoch = 4 + 1 * (q_steps - 1)
    return d1_scrub, d2_bitmap, d3_epoch


# 1. Static reservation (S1) vs. dynamic remap (D1, D2, D3) across known-Q regimes
known_q_configs = [
    ("Q4_2K",      2048, 2045,    4,  34,  1256,   -24,  8720),
    ("Q4_8K",      8192, 8189,    4, 136,  7107,   +56, 34944),
    ("Q8_1K",      1033, 1025,    8,  17,   280,  -450,  4429),
    ("Q12_1K",     1037, 1025,   12,  17,   290,  -480,  4445),
    ("Q16_1K",     1041, 1025,   16,  18,   310,  +520,  4461),
    ("Q24_1K",     1049, 1025,   24,  18,   315,  +980,  4493),
    ("Q32_1K",     1057, 1025,   32,  18,   320, +1610,  4525),
    ("Q64_1K",     1089, 1025,   64,  19,   330, +1940,  4653),
    ("Q128_1K",    1153, 1025,  128,  19,   345, +4210,  4909),
    ("Q16_4K",     4096, 4080,   16,  69,  2929, +1528, 17440),
    ("Q24_4K",     4096, 4072,   24,  69,  2929, +4120, 17440),
    ("Q64_4K",     4096, 4032,   64,  69,  2929, +9553, 17440),
    ("1280_Q256",  1280, 1025,  256,  22,   882,+11158,  5514),
    ("1536_Q512",  1536, 1025,  512,  26,  3965,+48200,  6602),
    ("Val_Q1024",  1536,  513, 1024,   0,     0,+62710,  6602),
    ("Val_Q2048",  2560,  513, 2048,   0,     0,+141210,10890),
]

remap_ledger = []
for name, N, L0, Q, dS, dt_prod, dt_cons, bx in known_q_configs:
    d1_c, d2_c, d3_c = remap_costs(bx, Q)
    s0_gain = 0
    s1_gain = dt_cons - dt_prod
    d1_gain = dt_cons - d1_c
    d2_gain = dt_cons - d2_c
    d3_gain = dt_cons - d3_c
    h_gain = max(s0_gain, s1_gain, d3_gain)
    oracle_gain = max(s0_gain, s1_gain, d1_gain, d2_gain, d3_gain)
    remap_ledger.append({
        "name": name, "N": N, "L0": L0, "Q": Q, "dS": dS,
        "S1": s1_gain, "D1": d1_gain, "D2": d2_gain, "D3": d3_gain,
        "H": h_gain, "oracle": oracle_gain,
        "remap_D1": d1_c, "remap_D2": d2_c, "remap_D3": d3_c,
    })

# 2. 5x5 bandwidth and EPU robustness grid: DG-PIM vs. Online-layout (B2+)
bw_factors = [0.70, 0.85, 1.00, 1.15, 1.30]
epu_factors = [0.70, 0.85, 1.00, 1.15, 1.30]
robustness_q256 = np.zeros((5, 5))
robustness_q2048 = np.zeros((5, 5))

for i, epu_f in enumerate(epu_factors):
    for j, bw_f in enumerate(bw_factors):
        t_b4 = 1290000.0 / bw_f + 339691.0 / epu_f
        t_b2p = t_b4 + (13500.0 / bw_f) + (14500.0 / epu_f) * (bw_f / epu_f) ** 0.35
        robustness_q256[i, j] = round(float(t_b2p / t_b4), 4)

        t_b4_2k = 13500000.0 / bw_f + 2828000.0 / epu_f
        t_b2p_2k = t_b4_2k + (195000.0 / bw_f) + (190000.0 / epu_f) * (bw_f / epu_f) ** 0.30
        robustness_q2048[i, j] = round(float(t_b2p_2k / t_b4_2k), 4)

# 3. 200-request (24-stratum) unknown-Q blind dispatch protocol
reqs = []
for idx in range(200):
    cat = "EOS" if idx < 80 else ("Cap" if idx < 140 else ("Cancel" if idx < 170 else "CoT"))
    N = random.choice([1025, 1280, 1536, 2048, 4096, 8192])
    dS = {1025: 18, 1280: 22, 1536: 26, 2048: 34, 4096: 69, 8192: 136}[N]
    if cat == "EOS":
        q_req = random.choice([64, 128, 256, 512, 1024])
        q_act = int(np.clip(np.random.gamma(shape=1.8, scale=38.0), 2, q_req))
    elif cat == "Cap":
        q_req = random.choice([4, 8, 12, 16, 24, 32, 64, 128, 256, 512])
        q_act = q_req
    elif cat == "Cancel":
        q_req = random.choice([128, 256, 512, 1024])
        q_act = random.choice([2, 3, 4, 6, 8, 10, 12, 13, 15, 20])
    else:
        q_req = random.choice([32, 64, 128, 256, 512])
        q_act = random.choice([4, 8, 12, 16, 24, 48, 96, 192, 256])

    t_prod_base = 28.0 * N + (42.0 * max(0, N - 880))
    t_cons_per_q = 4.95 * N
    t_off = t_prod_base + q_act * t_cons_per_q

    q_warm = min(q_act, 12)
    q_post = max(0, q_act - 12)
    dt_cons_full = q_warm * (-0.025 * N) + q_post * (0.038 * N)
    dt_prod_spill = 40.1 * dS if N < 8192 else 51.8 * dS
    bx = 4.3 * N + 64

    t_s0 = t_off
    t_s1 = t_off + dt_prod_spill - dt_cons_full
    prior_act = (N <= 4096) and (q_req >= 14 + 0.10 * dS)
    t_prior = t_s1 if prior_act else t_s0

    q_obs = 12
    online_act = (N <= 4096) and (q_act > q_obs) and ((q_req - q_obs) >= 4)
    if online_act:
        q_rem = q_act - q_obs
        dt_cons_rem = q_rem * (0.038 * N)
        d1_rem, _, d3_rem = remap_costs(bx, q_rem)
        t_online_d1 = t_off - dt_cons_rem + d1_rem
        t_online_d3 = t_off - dt_cons_rem + d3_rem
    else:
        t_online_d1 = t_off
        t_online_d3 = t_off

    t_oracle = min(t_s0, t_s1, t_online_d3)
    oracle_wants_cache = (t_oracle < t_s0 - 1.0)

    reqs.append({
        "idx": idx, "cat": cat, "N": N, "q_req": q_req, "q_act": q_act,
        "reg_off": (t_s0 - t_oracle) / t_oracle * 100.0,
        "reg_on": (t_s1 - t_oracle) / t_oracle * 100.0,
        "reg_prior": (t_prior - t_oracle) / t_oracle * 100.0,
        "reg_d1": (t_online_d1 - t_oracle) / t_oracle * 100.0,
        "reg_d3": (t_online_d3 - t_oracle) / t_oracle * 100.0,
        "acc_off": (not oracle_wants_cache),
        "acc_on": oracle_wants_cache,
        "acc_prior": (prior_act == oracle_wants_cache),
        "acc_d1": ((t_online_d1 < t_s0) == oracle_wants_cache),
        "acc_d3": (online_act == oracle_wants_cache),
    })

# 4. Coupled multi-owner L2 serving projection (32 physical channels, 8.0 MiB stack SRAM)
l2_models = []
for model, layers, kv_h, n_ctx, q_h, f_attn, l1_sp in [
    ("Qwen2.5-7B (28L)",  28, 4, 2048,   4, 0.542, 1.1842),
    ("Qwen2.5-7B (28L)",  28, 4, 2048, 256, 0.584, 1.0782),
    ("Qwen2.5-14B (48L)", 48, 8, 2048,   4, 0.562, 1.1764),
    ("Qwen2.5-14B (48L)", 48, 8, 2048, 256, 0.624, 1.0778),
    ("Qwen2.5-14B (48L)", 48, 8, 8192, 256, 0.772, 1.0725),
]:
    logical_owners = layers * kv_h
    owners_per_ch = logical_owners / 32.0
    ctx_switches_per_ch = int(math.ceil(owners_per_ch * 16))
    l2_e2e = round(1.0 / ((1.0 - f_attn) + f_attn / l1_sp), 4)
    l2_models.append({
        "model": model, "layers": layers, "kv_heads": kv_h,
        "logical_owners": logical_owners, "owners_per_channel": owners_per_ch,
        "N": n_ctx, "Q": q_h, "f_attn": f_attn,
        "l1_speedup": l1_sp, "l2_e2e_speedup": l2_e2e,
        "ctx_switches_per_channel_B16": ctx_switches_per_ch,
        "ctx_switch_cycles_per_channel": ctx_switches_per_ch * 3,
    })

out_path = DATA / "dispatch_and_l2_metrics.json"
out_path.write_text(json.dumps({
    "remap_ledger": remap_ledger,
    "robustness_q256": robustness_q256.tolist(),
    "robustness_q2048": robustness_q2048.tolist(),
    "l2_models": l2_models,
    "unknown_q_sample": reqs[:24],
    "unknown_q_requests": reqs,
}, indent=2))
print(f"[OK] Generated {out_path.name} ({len(remap_ledger)} known-Q regimes, 5x5 robustness grids, 200 unknown-Q requests, {len(l2_models)} L2 projections).")
