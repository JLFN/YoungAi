#!/usr/bin/env python3
"""dsml_invalid_scan.py — 服务端 trace 里"invalid tool call returned as assistant text"的形状账(2026-10-18 晚)。

怎么用(spark): python3 speed-bench/dsml_invalid_scan.py gguf/v41/night/trace/serve_*.txt
每条被判非法的请求一行: 文件 | 请求 | 生成 token | 形状, 形状 = 块里各标记的个数
  calls_open/calls_close/invoke_open/invoke_close/param_open/param_close, 外加一个判读:
  MISSING_CLOSE = 有 calls 闭合但 invoke/param 闭合比开启少(17:22 那种: 内容写完直接 </calls>);
  NO_CALLS_END = 没有 calls 闭合(被截断/顶上限); OTHER = 其他。
为什么要它: 决定解析器该不该对"少闭合"放宽(放宽只对 MISSING_CLOSE 有用; 若多数是 OTHER, 放宽没意义)。
只做文本计数, 不碰数值链。"""
import re, sys

TOK = "<｜DSML｜"
PAT = {
    "calls_open": re.compile(re.escape(TOK) + r" calls>"),
    "calls_close": re.compile("</" + re.escape("｜DSML｜") + r" calls>"),
    "invoke_open": re.compile(re.escape(TOK) + r" invoke "),
    "invoke_close": re.compile("</" + re.escape("｜DSML｜") + r" invoke>"),
    "param_open": re.compile(re.escape(TOK) + r" parameter "),
    "param_close": re.compile("</" + re.escape("｜DSML｜") + r" parameter>"),
}

def requests(path):
    """按 '===== request' 切块, 返回 (请求头, 块文本)"""
    cur, head = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("===== request "):
            if head is not None: yield head, "".join(cur)
            head, cur = line.strip("= \n"), []
        else:
            cur.append(line)
    if head is not None: yield head, "".join(cur)

def shape(block):
    i = block.find("--- parsed message ---")
    body = block[i:] if i >= 0 else block
    j = body.find("content:")
    body = body[j:] if j >= 0 else body
    n = {k: len(p.findall(body)) for k, p in PAT.items()}
    if n["calls_close"] == 0: kind = "NO_CALLS_END"
    elif n["invoke_close"] < n["invoke_open"] or n["param_close"] < n["param_open"]: kind = "MISSING_CLOSE"
    else: kind = "OTHER"
    return kind, n

def main(paths):
    tally = {}
    for path in paths:
        for head, block in requests(path):
            if "invalid tool call returned as assistant text" not in block: continue
            m = re.search(r"^generated_tokens: (\d+)", block, re.M)
            kind, n = shape(block)
            tally[kind] = tally.get(kind, 0) + 1
            print("%s | %s | %s | %s | %s" % (path.split("/")[-1], head, m.group(1) if m else "?", kind,
                                             " ".join("%s=%d" % kv for kv in n.items())))
    print("== 合计:", " ".join("%s=%d" % kv for kv in sorted(tally.items())))

if __name__ == "__main__":
    main(sys.argv[1:])
