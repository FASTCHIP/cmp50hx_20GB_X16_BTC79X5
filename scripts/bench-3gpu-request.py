#!/usr/bin/env python3
import json
import os
import sys
import time
import urllib.request

if len(sys.argv) != 4:
    raise SystemExit("usage: bench_request.py LABEL REP OUTPUT_JSON")
label, rep, output = sys.argv[1:]
key = open('/etc/llama-server.api-key', encoding='utf-8').read().strip()
# Unique marker prevents an accidental persistent prompt-cache hit; the body gives 128K-class PP.
prompt = (
    "Read the supplied context and output exactly integers 1 through 256, separated only by single spaces. "
    f"Benchmark nonce={label}-{rep}-{time.time_ns()}. Context:" + " word" * 128000
)
payload = {
    "model": "Qwen3.8-27B",
    "messages": [{"role": "user", "content": prompt}],
    "max_tokens": 512,
    "temperature": 0,
    "ignore_eos": True,
    "chat_template_kwargs": {"enable_thinking": False},
}
request = urllib.request.Request(
    "http://127.0.0.1:8081/v1/chat/completions",
    data=json.dumps(payload).encode(),
    headers={"Content-Type": "application/json", "Authorization": "Bearer " + key},
)
started_epoch = time.time()
started = time.monotonic()
with urllib.request.urlopen(request, timeout=900) as response:
    body = json.load(response)
elapsed = time.monotonic() - started
record = {
    "label": label,
    "rep": int(rep),
    "started_epoch": started_epoch,
    "elapsed_s": round(elapsed, 3),
    "usage": body.get("usage", {}),
    "finish_reason": body["choices"][0].get("finish_reason"),
    "content_len": len(body["choices"][0].get("message", {}).get("content") or ""),
}
with open(output, "w", encoding="utf-8") as f:
    json.dump(record, f, ensure_ascii=False)
print(json.dumps(record, ensure_ascii=False))
