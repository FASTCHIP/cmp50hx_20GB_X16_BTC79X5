#!/usr/bin/env bash
# Serial PP/decode matrix. Runs under root via systemd-run; restores the pre-test unit on every exit.
set -Eeuo pipefail

UNIT=llama-qwen.service
UNIT_PATH=/etc/systemd/system/llama-qwen.service
ROOT=/home/fastchip/llama-pp-decode-research
REQUEST="$ROOT/bench_request.py"
OUT="$ROOT/results-$(date -u +%Y%m%dT%H%M%SZ)"
ORIG="$OUT/original-${UNIT_PATH##*/}"
mkdir -p "$OUT"
cp -a "$UNIT_PATH" "$ORIG"
cp -a "$ROOT/RESEARCH-PLAN.md" "$OUT/" 2>/dev/null || true
printf 'started_utc=%s\n' "$(date -u --iso-8601=seconds)" | tee "$OUT/RUNNING"

restore() {
  rc=$?
  cp -a "$ORIG" "$UNIT_PATH"
  systemctl daemon-reload || true
  systemctl restart "$UNIT" || true
  for i in $(seq 1 150); do
    key=$(< /etc/llama-server.api-key)
    if curl -fsS --max-time 3 -H "Authorization: Bearer $key" http://127.0.0.1:8081/slots > "$OUT/final-slots.json" 2>/dev/null; then
      break
    fi
    sleep 2
  done
  printf 'finished_utc=%s rc=%s\n' "$(date -u --iso-8601=seconds)" "$rc" > "$OUT/DONE"
  rm -f "$ROOT/RUNNING"
  exit "$rc"
}
trap restore EXIT INT TERM
ln -sfn "$OUT" "$ROOT/RUNNING"

apply_variant() {
  local label=$1 batch=$2 ubatch=$3 t_batch=$4 split=$5
  python3 - "$ORIG" "$UNIT_PATH" "$batch" "$ubatch" "$t_batch" "$split" <<'PY'
import sys
src, dst, batch, ubatch, t_batch, split = sys.argv[1:]
s = open(src, encoding='utf-8').read()
needle = ' --api-key-file /etc/llama-server.api-key'
addition = f' -t 6 -tb {t_batch} -b {batch} -ub {ubatch}'
assert s.count(needle) == 1, s.count(needle)
assert ' -b ' not in s and ' -ub ' not in s and ' -tb ' not in s
if split == 'layer':
    assert s.count('--split-mode tensor') == 1
    s = s.replace('--split-mode tensor', '--split-mode layer')
elif split != 'tensor':
    raise SystemExit(split)
s = s.replace(needle, addition + needle)
open(dst, 'w', encoding='utf-8').write(s)
PY
  systemctl daemon-reload
  systemctl restart "$UNIT"
  local ready=0 key slots
  for i in $(seq 1 150); do
    key=$(< /etc/llama-server.api-key)
    slots=$(curl -fsS --max-time 3 -H "Authorization: Bearer $key" http://127.0.0.1:8081/slots 2>/dev/null || true)
    if [ -n "$slots" ]; then printf '%s\n' "$slots" > "$OUT/${label}-ready-slots.json"; ready=1; break; fi
    sleep 2
  done
  [ "$ready" = 1 ] || { journalctl -u "$UNIT" -n 200 --no-pager > "$OUT/${label}-startup-failure.log"; return 1; }
  systemctl show "$UNIT" -p ExecStart --value > "$OUT/${label}-execstart.txt"
  journalctl -u "$UNIT" -n 200 --no-pager -o cat > "$OUT/${label}-startup.log"
}

run_rep() {
  local label=$1 rep=$2
  local started_epoch
  started_epoch=$(date +%s)
  # Capture lightweight telemetry; it terminates after the client call.
  (
    while kill -0 "$PPID" 2>/dev/null; do
      date -u +%s
      nvidia-smi --query-gpu=index,utilization.gpu,utilization.memory,clocks.sm,power.draw --format=csv,noheader
      sleep 1
    done
  ) > "$OUT/${label}-rep${rep}-gpu.csv" &
  local sampler=$!
  "$REQUEST" "$label" "$rep" "$OUT/${label}-rep${rep}-response.json" > "$OUT/${label}-rep${rep}-client.json"
  kill "$sampler" 2>/dev/null || true
  wait "$sampler" 2>/dev/null || true
  sleep 2
  journalctl -u "$UNIT" --since "@$started_epoch" --no-pager -o cat > "$OUT/${label}-rep${rep}-journal.log"
  python3 - "$OUT/${label}-rep${rep}-response.json" "$OUT/${label}-rep${rep}-journal.log" "$label" "$rep" >> "$OUT/results.jsonl" <<'PY'
import json, re, sys
response, journal, label, rep = sys.argv[1:]
r = json.load(open(response))
x = open(journal, errors='replace').read()
pp = re.findall(r'prompt eval time =\s*([0-9.]+) ms /\s*(\d+) tokens \([^)]*,\s*([0-9.]+) tokens per second\)', x)
tg = re.findall(r'\beval time =\s*([0-9.]+) ms /\s*(\d+) tokens \([^)]*,\s*([0-9.]+) tokens per second\)', x)
if not pp or not tg:
    raise SystemExit('missing server timing line')
r.update({'label': label, 'rep': int(rep), 'server_pp_ms': float(pp[-1][0]), 'server_prompt_tokens': int(pp[-1][1]), 'pp_tok_s': float(pp[-1][2]), 'server_tg_ms': float(tg[-1][0]), 'server_completion_tokens': int(tg[-1][1]), 'decode_tok_s': float(tg[-1][2])})
print(json.dumps(r, ensure_ascii=False))
PY
}

# Each non-baseline row has exactly one changed tuning domain relative to the explicit baseline.
# layer is intentionally deferred because it changes the multi-GPU execution model.
for spec in \
  'B0_explicit-defaults 2048 512 6 tensor' \
  'V1_batch4096 4096 512 6 tensor' \
  'V2_ubatch1024 2048 1024 6 tensor' \
  'V3_threads_batch10 2048 512 10 tensor' \
  'V4_layer-split 2048 512 6 layer'
do
  read -r label batch ubatch t_batch split <<< "$spec"
  printf 'variant=%s begin=%s\n' "$label" "$(date -u --iso-8601=seconds)" | tee -a "$OUT/progress.log"
  apply_variant "$label" "$batch" "$ubatch" "$t_batch" "$split"
  for rep in 1 2 3; do
    printf 'variant=%s rep=%s begin=%s\n' "$label" "$rep" "$(date -u --iso-8601=seconds)" | tee -a "$OUT/progress.log"
    run_rep "$label" "$rep"
  done
  printf 'variant=%s end=%s\n' "$label" "$(date -u --iso-8601=seconds)" | tee -a "$OUT/progress.log"
done

python3 - "$OUT/results.jsonl" > "$OUT/summary.txt" <<'PY'
import json, statistics, sys
rows = [json.loads(x) for x in open(sys.argv[1]) if x.strip()]
print('rows=', len(rows))
for label in dict.fromkeys(r['label'] for r in rows):
    a = [r for r in rows if r['label'] == label]
    print(label, 'n=',len(a), 'pp_median=',round(statistics.median(r['pp_tok_s'] for r in a),2), 'decode_median=',round(statistics.median(r['decode_tok_s'] for r in a),2), 'wall_median_s=',round(statistics.median(r['elapsed_s'] for r in a),2))
PY
