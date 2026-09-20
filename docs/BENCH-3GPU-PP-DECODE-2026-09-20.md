# 3× CMP 50HX: PP/decode trade-off matrix — 2026-09-20

This report records the controlled experiment used to answer a practical question:
**can prompt-processing (PP) throughput increase materially without degrading decode
throughput?**

## Result in one line

No tested configuration met both gates: **PP ≥ 5% above baseline** and **decode ≥ 95%
of baseline**.

- The safe runtime change, `--ubatch-size 1024`, improved PP only **1.15%**.
- Three-GPU layer split raised PP **46.60%**, but reduced decode **18.80%**.
- The production configuration was restored unchanged: 3-GPU tensor split with NCCL/P2P.

## Test environment

| Item | Value |
|---|---|
| Host | `ai100gb` |
| CPU | Xeon E5-2620 v1, 6C/12T, Sandy Bridge |
| GPUs | GPU0–2: 3× CMP 50HX (TU102/SM75), PCIe Gen2 x8 |
| Interconnect | Every GPU pair is PHB; no NVLink |
| P2P | CUDA peer copies passed for 0→1, 0→2, and 1→2 |
| llama.cpp | exact source commit `81f670c7e2025da4345b719a69ed9ce2f5c367db`; NCCL-linked build |
| Model | `Qwen3.8-27B-UD-Q4_K_XL.gguf` with DFlash2 draft model |
| Context / slots | 262144 / 1; unified `q8_0` KV |
| Fixed settings | FlashAttention on; CUDA Graph optimization on; GPU layers 99; DFlash fixed |

## Method

- A single `llama-server` was used on `0.0.0.0:8081`.
- Each variant changed one parameter domain only, then reloaded the model.
- For each variant: authenticated health check, **three independent uncached** requests,
  each approximately **128067 prompt tokens → 512 completion tokens**.
- PP and decode are the server's own `prompt eval` and `eval` timing values, not client-side
  estimates. The table uses the median of three runs.
- GPU telemetry was sampled once per second during every request.
- The runner backed up the active unit, used an `EXIT` trap to restore it on error or stop,
  and checked API health after every restart.
- The logs were checked for `CUDA error`, `NCCL error`, OOM, Xid, and AER indicators.
  None were found.

The raw 15-run records are committed at
[`data/benchmarks/2026-09-20-3gpu-pp-decode/results.jsonl`](../data/benchmarks/2026-09-20-3gpu-pp-decode/results.jsonl).
The reproducibility runner and request harness are in
[`scripts/bench-3gpu-pp-decode-matrix.sh`](../scripts/bench-3gpu-pp-decode-matrix.sh) and
[`scripts/bench-3gpu-request.py`](../scripts/bench-3gpu-request.py).

## Variants and measured medians

| ID | Single delta from baseline | PP tok/s | PP delta | Decode tok/s | Decode delta | Median wall time |
|---|---|---:|---:|---:|---:|---:|
| B0 | Tensor, `-b 2048 -ub 512 -t 6 -tb 6` | 357.16 | — | 53.78 | — | 382.70 s |
| V1 | `-b 4096` | 358.49 | +0.37% | 53.82 | +0.07% | 381.69 s |
| V2 | `-ub 1024` | 361.28 | +1.15% | 53.86 | +0.15% | 379.06 s |
| V3 | `-tb 10` | 357.04 | -0.03% | 53.90 | +0.22% | 382.95 s |
| V4 | `--split-mode layer`, three GPUs | 523.60 | +46.60% | 43.67 | -18.80% | 270.62 s |

## Interpretation

### Tensor split remains the decode configuration

In tensor mode, each decode token involves cross-GPU collective work. The tested GPUs have
P2P enabled, but their PCIe traffic still travels through the host bridge. Tensor split is
therefore the better choice when interactive decode latency matters.

Increasing `-b` to 4096 and batch CPU threads to 10 did not create a material PP change.
Increasing `-ub` to 1024 was harmless and repeatably small (+1.15% PP), but does not justify
a production policy change by itself.

### Layer split is the PP configuration

Layer split removes most tensor-parallel collective traffic during prompt processing. On this
hardware it raised PP from 357.16 to 523.60 tok/s. Decode becomes a pipeline through the
three devices and fell from 53.78 to 43.67 tok/s, so it fails the decode gate.

This establishes the trade-off rather than a universal winner:

- choose **tensor** for the deployed interactive endpoint;
- choose **layer** only for a dedicated bulk-prefill/batch endpoint where roughly 19% lower
  decode speed is acceptable in exchange for roughly 47% higher PP.

## Production state after test

The benchmark exited successfully and restored the existing service. Verification immediately
afterward showed:

```text
CUDA_VISIBLE_DEVICES=0,1,2
--split-mode tensor --tensor-split 1,1,1
/slots: healthy
one llama-server process resident on GPU0, GPU1, and GPU2
```

No benchmark setting was promoted to the production unit.

## Next experiment, if required

The only evidence-led next branch is layer-pipeline balancing: test individual layer
allocation ratios separately, keeping the same 128K/512 workload and the same decode gate.
There is no evidence yet that any ratio will retain the layer PP gain while recovering decode;
it is a hypothesis, not a recommended configuration.
