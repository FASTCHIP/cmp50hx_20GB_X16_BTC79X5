# LLM Serving — llama.cpp on 2× CMP 50HX

Serving documentation for the production 2× CMP 50HX (20 GiB) host.
Updated 2026-09-18: **vLLM was fully removed** (see *Removed: vLLM* below);
the llama.cpp units are installed but disabled — enable one to serve.

## Host constraints that shape these configs

- 2× CMP 50HX 20 GiB (TU102, `10de:1e09`, sm_75), driver 610.43.03.
- 15 GiB system RAM; models on slow USB-SATA (`/mnt/usbsata/models`) —
  multi-GB loads take minutes.
- The GPUs come up only after the unlock chain (`cmp50hx-gen2-rescan` →
  `cmp50hx-gen2` → `cmp-idle-governor`, see [SETUP-ubuntu-24.04.md](SETUP-ubuntu-24.04.md));
  serving units MUST order after it.

## Removed: vLLM (2026-09-18)

vLLM 0.29.0 (`vllm-qwen.service`, port 8091, Qwen3.8-27B-AWQ) was uninstalled
from the host: the service was stopped and disabled, the unit file deleted,
and `~/vllm-qwen38` (venv + serve/bench scripts), `/opt/vllm-fork`,
`~/.cache/vllm` and `~/vllm-8092-cmdline.txt` removed. Verified: no vLLM
units or processes left, port 8091 free, both GPUs at 0 MiB.

Rollback / reference material:

- Backup on the local workstation (NOT on this host):
  `~/vllm-removal-backup-20260918` — unit file, all `serve-*.sh` variants,
  bench scripts, `vllm.pid`, plus the incident logs `vllm-qwen-journal.txt`
  and `stop-vllm.txt` (copied from `~/cmp50-gpu1-drop-2026-09-18/`).
- The complete verified vLLM configuration as it read before removal is in
  this file's git history (commit `8fb4fbf`); benchmark numbers:
  [BENCH-QWEN38.md](BENCH-QWEN38.md).
- Model weights `Qwen3.8-27B-AWQ` (21 GiB) are still under
  `/mnt/usbsata/models/` — remove separately if no longer wanted.

## llama.cpp — installed, disabled (`llama-qwen` :8081, `llama-ornith` :8082)

Both units are disabled; with vLLM gone this is the available serving stack.

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

1. `systemctl status cmp50hx-gen2-rescan cmp50hx-gen2 cmp-idle-governor` — all active.
2. `nvidia-smi` lists both cards; link speed ≥ 5.0 GT/s on both.
3. If a llama.cpp unit was enabled: `ss -tln | grep 8081` (or `8082`), then
   `curl -s http://127.0.0.1:8081/v1/models` — allow extra time for the first
   multi-GB model load from USB-SATA. (vLLM's port 8091 no longer exists.)

## Troubleshooting

- **Link stuck at Gen1 x4** — read TLS with `setpci -s <bdf> CAP_EXP+30.W`;
  retrain only once TLS reads 2, earlier retrains fail because the registers
  are still locked (handled by `cmp50hx-gen2`).
- **GPU1 drops** — evidence from the 2026-09-18 incident (dmesg, journals,
  nvidia bug report) is in `~/cmp50-gpu1-drop-2026-09-18/` on the host.
- `postgresql@16-main` sitting in failed state is chronic on this host and
  unrelated to LLM serving.

## Benchmarking

[BENCH-QWEN38.md](BENCH-QWEN38.md) holds historical vLLM benchmark numbers
(collected before the 2026-09-18 removal) plus the methodology; benchmark
script: [`scripts/bench-qwen38.py`](../scripts/bench-qwen38.py).
