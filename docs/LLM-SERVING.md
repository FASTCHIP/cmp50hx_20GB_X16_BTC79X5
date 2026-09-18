# LLM Serving — vLLM & llama.cpp on 2× CMP 50HX

Verified serving configurations for the production 2× CMP 50HX (20 GiB) host.
vLLM is production and owns the GPUs; the llama.cpp units are installed but
disabled. Validated September 2026.

## Host constraints that shape these configs

- 2× CMP 50HX 20 GiB (TU102, `10de:1e09`, sm_75), driver 610.43.03.
- 15 GiB system RAM; models on slow USB-SATA (`/mnt/usbsata/models`) —
  multi-GB loads take minutes.
- The GPUs come up only after the unlock chain (`cmp50hx-gen2-rescan` →
  `cmp50hx-gen2` → `cmp-idle-governor`, see [SETUP-ubuntu-24.04.md](SETUP-ubuntu-24.04.md));
  serving units MUST order after it.

## vLLM — production (`vllm-qwen.service`, port 8091)

**Version: vLLM 0.29.0** in a dedicated venv (`~/vllm-qwen38/venv`), started
via `~/vllm-qwen38/serve-safe.sh`. Model: Qwen3.8-27B-AWQ, served as
`Qwen3.8-27B`, listening on `0.0.0.0:8091`.

```
vllm serve $MODEL_DIR              # AWQ snapshot under /mnt/usbsata/models
  --served-model-name Qwen3.8-27B
  --host 0.0.0.0 --port 8091
  --tensor-parallel-size 2
  --dtype float16
  --max-model-len 131072
  --gpu-memory-utilization 0.90
  --kv-cache-dtype float16
  --enable-prefix-caching
  --enable-chunked-prefill
  --max-num-batched-tokens 16384
  --max-num-seqs 16
  --long-prefill-token-threshold 16384
  --enable-auto-tool-choice
  --tool-call-parser qwen3_xml
  --reasoning-parser qwen3
  --disable-custom-all-reduce
```

(The exact executable line and `$MODEL_DIR` live in `serve-safe.sh` on the host.)

Why these flags:

| Flag | Reason |
|---|---|
| `--tensor-parallel-size 2` | both cards pool ~40 GiB for one model |
| `--dtype float16` | fastest dequant path for AWQ on Turing (sm_75) |
| `--max-model-len 131072` | full 128K context; fp16 KV fits at 0.90 util |
| `--enable-prefix-caching` | large reuse win for system-prompt / agent traffic |
| `--enable-chunked-prefill` + `--max-num-batched-tokens 16384` | smooth long prefills, keep decode latency stable |
| `--long-prefill-token-threshold 16384` | let long prompts take the chunked path |
| `--max-num-seqs 16` | matches the KV budget on 20 GiB cards |
| `--disable-custom-all-reduce` | custom all-reduce kernels are unreliable on these cards; the NCCL path is stable |

**Mandatory env — without these the engine dies during model load.** It looks
like a crash, not an OOM: slow driver + USB-SATA exceed the default timeouts.

```
VLLM_EXECUTE_MODEL_TIMEOUT_SECONDS=1800
VLLM_ENGINE_ITERATION_TIMEOUT_S=1800
```

Unit wiring: `Restart=on-failure`, `RestartSec=15`, ordered after the unlock
chain:

```ini
[Unit]
Requires=cmp50hx-gen2-rescan.service
Wants=cmp50hx-gen2.service
After=cmp50hx-gen2.service
```

## llama.cpp — installed, disabled (`llama-qwen` :8081, `llama-ornith` :8082)

Both units are disabled while vLLM is production; the configs are kept ready
to re-enable.

### `llama-qwen.service` — port 8081

- Model: Qwen3.8-27B-UD-Q4_K_XL + mmproj Q8_0 (vision); GGUFs under
  `/mnt/usbsata/models/` (exact paths in `systemctl cat llama-qwen`).
- Binary: `~/llama.cpp-tensor-prod/bin/llama-server` with LD_LIBRARY_PATH on
  the same dir; env `GGML_CUDA_GRAPH_OPT=1`, `CUDA_VISIBLE_DEVICES=0,1`.

```
-m $MODEL --mmproj $MMPROJ
  --gpu-layers 999
  --split-mode tensor --tensor-split 1,1
  --flash-attn on
  --ctx-size 131072
  --cache-type-k q8_0 --cache-type-v q8_0
  --parallel 1
  --jinja
  --api-key-file /etc/llama-server.api-key
  --n-predict 12288
  --host 0.0.0.0 --port 8081
```

### `llama-ornith.service` — port 8082

- Model: Ornith-1.5-9B-Q4_K_M + mmproj BF16.
- `--split-mode layer`, KV cache `q4_0`.
- Binary: `~/llama.cpp-mtmd-fix-build/build-dp4a/bin/llama-server`.

### Build/split pairing rule

- Tensor split (`--split-mode tensor`) → the **tensor-prod** build.
- Layer split (`--split-mode layer`) → the **mtmd-fix** build.

Mixing a build with the wrong split mode misbehaves — always match them.

API key for both servers: `/etc/llama-server.api-key` (root:fastchip, 0640).
Both units carry a historical `Requires=docker.service`.

## Post-reboot verification

1. `systemctl status cmp50hx-gen2-rescan cmp50hx-gen2 cmp-idle-governor vllm-qwen` — all active.
2. `nvidia-smi` lists both cards; link speed ≥ 5.0 GT/s on both.
3. `ss -tln | grep 8091`, then `curl -s http://127.0.0.1:8091/v1/models` →
   `Qwen3.8-27B`. Allow 1–2 min of engine init after unit start (~30 s from
   boot in practice).

## Troubleshooting

- **Link stuck at Gen1 x4** — read TLS with `setpci -s <bdf> CAP_EXP+30.W`;
  retrain only once TLS reads 2, earlier retrains fail because the registers
  are still locked (handled by `cmp50hx-gen2`).
- **vLLM dies during model load** — the 1800 s env timeouts are missing; it
  is not an OOM.
- **GPU1 drops** — evidence from the 2026-09-18 incident (dmesg, journals,
  nvidia bug report) is in `~/cmp50-gpu1-drop-2026-09-18/` on the host.
- `postgresql@16-main` sitting in failed state is chronic on this host and
  unrelated to LLM serving.

## Benchmarking / tuning

Never restart prod in place to test a change: run one-delta variants on a
separate unit/port and health-gate before measuring. Methodology and
before/after numbers: [BENCH-QWEN38.md](BENCH-QWEN38.md); benchmark script:
[`scripts/bench-qwen38.py`](../scripts/bench-qwen38.py).
