# Qwen3.8-27B inference benchmark — real-workload impact of the ReBAR/P2P work

This repo is the firmware/driver/link work that unlocks 32 GiB BAR1, Gen2 x8, and BAR1
P2P on the CMP 50HX. Those milestones are verified with synthetic benchmarks
(2.47 GB/s peer DMA, `topo -p2p r = OK`, `Region 1 [size=32G]`). This doc answers the
follow-up that synthetic numbers cannot: **does any of it make the actual LLM workload
faster?**

**Answer: not meaningfully. Decode is compute-bound on the CMP 50HX (TU102, no tensor
cores), so BAR1 (64 MiB → 32 GiB) + P2P (off → on) moved the real Qwen3.8-27B workload
by ≤ 3 % — and ~1 % at 124K context.**

## Stand

| Parameter | During history (12–14.09) | Now (16.09) |
|---|---|---|
| GPU | 2× CMP 50HX, 20480 MiB each | same |
| Link | PCIe Gen2 x8 (already unlocked) | Gen2 x8 |
| BAR1 | 64 MiB stock (`cmp50_rebar_size=0`) | 32 GiB (selector 9 + 256 GiB MMIOH) |
| P2P | none (CNS) | OK (`topo -p2p r`) |
| Driver | 610.43.03 open + xrip patches | 610.43.03-p2p (aikitoria) |
| Model | Qwen3.8-27B-UD-Q4_K_XL.gguf (27.32 B) | same |
| KV / ctx / parallel | q8_0 / 131072 / 1 | same |
| Split | tensor 1,1 | tensor 1,1 |

The only deltas between "before" and "after" are BAR1 and P2P — Gen2 was already in
place during the history. The comparison therefore isolates exactly the ReBAR + P2P work.

## Method

`POST /completion` to llama-server, `n_predict=128`, `temperature=0`,
`cache_prompt=false`, `ignore_eos=true`; 3 runs, median. Prompt = file content + a fixed
instruction suffix. Prompts: p4k ≈ 4206, p32k ≈ 32649, p124k ≈ 125680 tokens.
Reproduction script: `scripts/bench-qwen38.py`.

## History (12–14.09) — before (64 MiB BAR1, no P2P)

### 12.09 — llama-bench (pp512 / tg128)

| Split | pp512 | tg128 |
|---|---|---|
| tensor | 341.17 ± 45.7 | 38.32 ± 0.90 |
| layer | 349.26 ± 52.0 | 25.76 ± 0.09 |

### 12.09 — full matrix (short prompt, gen t/s, mean of 5 runs)

| Mode | gen t/s |
|---|---|
| tensor | 36.59 |
| tensor + vision | 35.48 |
| tensor + dflash | 28.61 |
| tensor + dflash + vision | 26.72 |
| layer | 24.98 |
| layer + vision | 25.16 |
| layer + dflash | 24.00 |
| layer + dflash + vision | 26.95 |

### 14.09 — main A/B (median of 2–3 runs)

| Variant | p4k pp/tg | p32k pp/tg | p124k pp/tg |
|---|---|---|---|
| tensor, no draft | 523.8 / 36.55 | 510.9 / 30.49 | 411.5 / 20.83 |
| layer, no draft | 604.5 / 24.80 | 515.0 / 20.87 | 346.9 / 13.05 |
| tensor + DFlash2 n-max 5 | 448.9 / 37.94 | 444.6 / 30.71 | 366.6 / 21.30 |
| layer + DFlash2 n-max 5 | 618.7 / 34.16 | 575.2 / 28.98 | 410.6 / 19.06 |

Production profile chosen from this: **tensor 1,1, no draft** (tg 36.4–36.7 short,
30.5 @32K, 20.8 @124K; prefill 523.9 → 411.5 t/s).

Early probes (02–11.09) for completeness: dense Q4_K_XL split 1/1.6 KV q4_0 = 24.15 t/s
decode; fullctx 124K = prefill 379.6 / decode 12.85 t/s; vLLM rejected (60K prefill
2.4× slower than llama.cpp).

## Fresh measurement (16.09) — after (32 GiB BAR1, P2P)

Same method, against the running production server (tensor 1,1, no draft):

| Prompt | pp t/s | tg t/s |
|---|---|---|
| pp512/tg128 (~500 tok) | 422.6 | 37.07 |
| p4k (4206 tok) | 533.6 | 37.05 |
| p32k (32649 tok) | 526.5 | 30.87 |
| p124k (125680 tok) | 415.3 | 21.03 |

## Before/after — tensor, no draft

| Prompt | pp before | pp after | Δ | tg before | tg after | Δ |
|---|---|---|---|---|---|---|
| p4k | 523.8 | 533.6 | +1.9 % | 36.55 | 37.05 | +1.4 % |
| p32k | 510.9 | 526.5 | +3.1 % | 30.49 | 30.87 | +1.2 % |
| p124k | 411.5 | 415.3 | +0.9 % | 20.83 | 21.03 | +1.0 % |

## Conclusion

1. BAR1 64 MiB → 32 GiB + P2P off → on moved the real workload by **≤ 3 %**: decode
   ~1 %, prefill ~1–3 %, and only ~1 % at 124K (the long-context case that matters).
2. Decode is **compute-bound**, not link-bound: GPU utilization during decode is
   80–94 %, and decode throughput was flat (~36–38 tok/s) across the whole 12–16.09
   window regardless of link/BAR1/P2P state.
3. The CMP 50HX (TU102, **no tensor cores**) is the ceiling for this model; the PCIe
   link was never the bottleneck.
4. What the ReBAR/P2P work DID buy (see README): correct multi-GPU tensor-split (the
   mailbox fallback corrupts host RAM — now eliminated), faster prefill (single-digit
   %), 2× model load. Not faster decode.

**Rule of thumb**: after any link/BAR1/P2P change, sample
`nvidia-smi --query-gpu=utilization.gpu` during a decode run — ≥ 80 % means
compute-bound and the link work won't move the needle.
