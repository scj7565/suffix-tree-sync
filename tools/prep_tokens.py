"""tree_bench 입력 데이터 만들기: 실제 LLM 답변을 실제 토크나이저로 토큰화 -> data/tokens_<src>.bin
(보통은 팀 드라이브에 공유된 파일을 받아서 쓰면 되고, 이 스크립트는 다시 만들 때만 필요)

  python tools/prep_tokens.py --src code  --out data/tokens_code.bin   # Magicoder-OSS-Instruct-75K (LLM이 쓴 코드 답변)
  python tools/prep_tokens.py --src chat  --out data/tokens_chat.bin   # UltraChat-200k test_sft (LLM 대화 답변)
  python tools/prep_tokens.py --src files:<dir> --out data/tokens_files.bin   # 오프라인 대체: 폴더 안 소스 파일

Each record = (prompt tokens, response tokens). Responses shorter than --min-resp are dropped.
Format: b"KTOK" (파일 형식 표시), int32 n, then n x (int32 plen, int32 rlen, int32[plen+rlen]).
pip install datasets tokenizers numpy
"""
import argparse
import glob
import os
import struct

import numpy as np
from tokenizers import Tokenizer

ap = argparse.ArgumentParser()
ap.add_argument("--src", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--n", type=int, default=11000)
ap.add_argument("--tokenizer", default="Qwen/Qwen2.5-0.5B-Instruct")
ap.add_argument("--min-resp", type=int, default=128)
ap.add_argument("--max-prompt", type=int, default=512)
ap.add_argument("--max-resp", type=int, default=1024)
args = ap.parse_args()

tok = Tokenizer.from_pretrained(args.tokenizer)


def pairs():
    if args.src == "code":
        from datasets import load_dataset
        for ex in load_dataset("ise-uiuc/Magicoder-OSS-Instruct-75K", split="train", streaming=True):
            yield ex["problem"], ex["solution"]
    elif args.src == "chat":
        from datasets import load_dataset
        for ex in load_dataset("HuggingFaceH4/ultrachat_200k", split="test_sft", streaming=True):
            m = ex["messages"]
            if len(m) >= 2 and m[0]["role"] == "user" and m[1]["role"] == "assistant":
                yield m[0]["content"], m[1]["content"]
    elif args.src.startswith("files:"):
        root = args.src[6:]
        for f in sorted(glob.glob(os.path.join(root, "**", "*.*"), recursive=True)):
            if not f.endswith((".py", ".c", ".cc", ".cpp", ".h", ".js", ".ts", ".java", ".md")):
                continue
            text = open(f, encoding="utf-8", errors="ignore").read()
            for s in range(0, len(text) - 4000, 4000):
                yield text[s:s + 600], text[s + 600:s + 4000]
    else:
        raise SystemExit("unknown --src")


recs = []
for p, r in pairs():
    pi = tok.encode(p).ids[-args.max_prompt:]
    ri = tok.encode(r).ids[:args.max_resp]
    if len(ri) < args.min_resp or len(pi) < 4:
        continue
    recs.append((pi, ri))
    if len(recs) % 1000 == 0:
        print(f"  {len(recs)} records")
    if len(recs) >= args.n:
        break

rng = np.random.default_rng(0)
order = rng.permutation(len(recs))
with open(args.out, "wb") as f:
    f.write(b"KTOK" + struct.pack("<i", len(recs)))
    for k in order:
        pi, ri = recs[k]
        f.write(struct.pack("<ii", len(pi), len(ri)))
        f.write(np.asarray(pi + ri, dtype="<i4").tobytes())
rl = [len(r) for _, r in recs]
print(f"wrote {args.out}: {len(recs)} records, resp tokens mean {np.mean(rl):.0f}, median {np.median(rl):.0f}")
