#!/usr/bin/env python3
"""Benchmark llama-server: prefill and decode separately, N runs, median.

Faithful replica of the benchmark used for docs/BENCH-QWEN38.md. POSTs to the native
/completion endpoint (it carries the timings block; the OpenAI-style
/v1/chat/completions reply does not).

Usage:
  python3 bench-qwen38.py --port 8081 --key-file /etc/llama-server.api-key \
      --prompt-file p4k.txt --n-predict 128 --repeat 3 --label tensor-nodeft-p4k
"""
import argparse
import json
import time
import urllib.request


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--key", default="", help="API key, or use --key-file")
    ap.add_argument("--key-file", default="")
    ap.add_argument("--prompt-file", required=True, help="prompt text file")
    ap.add_argument("--suffix", default="")
    ap.add_argument("--n-predict", type=int, default=128)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--label", default="bench")
    args = ap.parse_args()

    key = args.key or (open(args.key_file).read().strip() if args.key_file else "")
    prompt = open(args.prompt_file, encoding="utf-8").read()
    if args.suffix:
        prompt += "\n\n" + args.suffix

    def post(obj):
        req = urllib.request.Request(
            "http://%s:%d/completion" % (args.host, args.port),
            data=json.dumps(obj).encode(),
            headers={"Content-Type": "application/json",
                     "Authorization": "Bearer " + key})
        t0 = time.time()
        with urllib.request.urlopen(req, timeout=1200) as r:
            return json.loads(r.read()), time.time() - t0

    obj = {"prompt": prompt, "n_predict": args.n_predict, "temperature": 0,
           "cache_prompt": False, "stream": False, "ignore_eos": True}
    pps, tgs = [], []
    for i in range(1, args.repeat + 1):
        d, wall = post(obj)
        t = d.get("timings", {})
        pps.append(t.get("prompt_per_second", 0.0))
        tgs.append(t.get("predicted_per_second", 0.0))
        print("run%d: prompt_n=%s pp=%.2f t/s  gen_n=%s tg=%.2f t/s  wall=%.1fs" % (
            i, t.get("prompt_n"), pps[-1], t.get("predicted_n"), tgs[-1], wall),
            flush=True)

    def med(v):
        s = sorted(v)
        n = len(s)
        return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2

    print("MEDIAN %s: pp=%.2f t/s  tg=%.2f t/s" % (args.label, med(pps), med(tgs)))


if __name__ == "__main__":
    main()
