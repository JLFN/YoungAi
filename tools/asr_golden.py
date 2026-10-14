#!/usr/bin/env python3
"""语音识别金标夹具(2026-10-14): 用 HF 官方转换的 ONNX 参考实现(onnx-community/moonshine-tiny-zh-ONNX, fp32)对一段 WAV
产出编码器输出与逐步 logits, 给 ds4_test --asr 逐位比(src/model/asr/)。

只做夹具, 不参与任何数值链(全仓 Python 裁决: 只许做外部采集/金标生成)。在 Mac 上跑:
  ~/hfenv/bin/python tools/asr_golden.py --onnx gguf/asr/moonshine-tiny-zh-onnx --wav tests/fixtures/asr/demo.wav \
      --tok gguf/asr/moonshine-tiny-zh/tokenizer.json --out tests/fixtures/asr/demo.golden

解码与 C 侧同法: 贪心、每步把整段前缀喂给无缓存的 decoder_model 取末位 logits, bos=1 起, eos=2 停,
上限 = 样本数 × 13 / 16000(官方模型卡的 token_limit_factor)。

文件格式(小端): "ASRG" u32 ver=1 | u32 n_samples n_frames D n_steps V topk | f32 enc[n_frames*D]
                 | 每步: i32 chosen, topk × (i32 id, f32 logit)(按 logit 降序) | u32 text_len, utf8 text
"""
import argparse, struct, wave
import numpy as np
import onnxruntime as ort

TOPK = 16


def read_wav16k(path):
    with wave.open(path, "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == 16000, (w.getparams())
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    return pcm.astype(np.float32) / 32768.0


def decode(tok_path, ids):
    """tokenizer.json 的 decoder 链(Replace ▁→空格 → ByteFallback → Fuse → Strip 首空格)照抄; 本机没装 tokenizers 包。
    文本只是夹具里的"人读项", 门比的是 id 与 logits。"""
    import json
    vocab = {v: k for k, v in json.load(open(tok_path, encoding="utf-8"))["model"]["vocab"].items()}
    out = bytearray()
    for i in ids:
        p = vocab[i]
        if p in ("<unk>", "<s>", "</s>") or (p.startswith("<<") and p.endswith(">>")):
            continue
        if len(p) == 6 and p.startswith("<0x") and p.endswith(">"):
            out.append(int(p[3:5], 16))
        else:
            out += p.replace("▁", " ").encode("utf-8")
    return out.decode("utf-8", errors="replace").lstrip(" ")


def load_st(path):
    """safetensors → {名: np.ndarray}(f32)。只为下面的 numpy 参考用。"""
    import json
    with open(path, "rb") as f:
        n = struct.unpack("<Q", f.read(8))[0]
        hdr = json.loads(f.read(n))
        base = 8 + n
        f.seek(0)
        blob = f.read()
    out = {}
    for k, v in hdr.items():
        if k == "__metadata__":
            continue
        a, b = v["data_offsets"]
        out[k] = np.frombuffer(blob[base + a:base + b], dtype="<f4").reshape(v["shape"])
    return out


def ref_encoder(W, cfg, x, rope):
    """numpy 参考编码器(与 src/model/asr/asr_graph.c 同一条算式), 用来①判定 RoPE 配对方式(对 ONNX 终输出)②给 C 侧逐段对账。
    rope = 'half'(Llama rotate_half: 第 i 维配 i+16) 或 'inter'(相邻配对: 2i 配 2i+1)。返回各段: stem / l0 / enc。"""
    from numpy.lib.stride_tricks import sliding_window_view as swv
    D, H = cfg["hidden_size"], cfg["encoder_num_attention_heads"]
    Dh, eps = D // H, 1e-5
    rot = int(Dh * cfg["partial_rotary_factor"]); theta = cfg["rope_theta"]
    gelu = lambda v: 0.5 * v * (1.0 + _erf(v / np.sqrt(2.0)))
    w1 = W["model.encoder.conv1.weight"][:, 0, :]                      # [D][127]
    h = np.tanh(swv(x, w1.shape[1])[::64] @ w1.T)                       # [t1][D]
    m, v = h.mean(), h.var()
    h = (h - m) / np.sqrt(v + eps) * W["model.encoder.groupnorm.weight"] + W["model.encoder.groupnorm.bias"]
    w2 = W["model.encoder.conv2.weight"]                                # [2D][D][7]
    win = swv(h, w2.shape[2], axis=0)[::3]                               # [t2][D][7]
    h = gelu(np.einsum("tcj,ocj->to", win, w2) + W["model.encoder.conv2.bias"])
    w3 = W["model.encoder.conv3.weight"]                                # [D][2D][3]
    win = swv(h, w3.shape[2], axis=0)[::2]
    h = gelu(np.einsum("tcj,ocj->to", win, w3) + W["model.encoder.conv3.bias"])
    stem = h.copy()
    T = h.shape[0]
    pos = np.arange(T, dtype=np.float64)
    inv = theta ** (-np.arange(0, rot, 2, dtype=np.float64) / rot)        # [rot/2]
    ang = pos[:, None] * inv[None, :]
    c, s = np.cos(ang)[:, None, :], np.sin(ang)[:, None, :]              # [T][1][rot/2]

    def ln(v, w): return (v - v.mean(-1, keepdims=True)) / np.sqrt(v.var(-1, keepdims=True) + eps) * w

    def rope_f(q):   # q [T][H][Dh]
        q = q.copy()
        if rope == "half":
            x1, x2 = q[..., :rot // 2].copy(), q[..., rot // 2:rot].copy()
            q[..., :rot // 2] = x1 * c - x2 * s
            q[..., rot // 2:rot] = x2 * c + x1 * s
        else:
            x1, x2 = q[..., 0:rot:2].copy(), q[..., 1:rot:2].copy()
            q[..., 0:rot:2] = x1 * c - x2 * s
            q[..., 1:rot:2] = x2 * c + x1 * s
        return q

    l0 = None
    for l in range(cfg["encoder_num_hidden_layers"]):
        p = f"model.encoder.layers.{l}."
        xn = ln(h, W[p + "input_layernorm.weight"])
        q = rope_f((xn @ W[p + "self_attn.q_proj.weight"].T).reshape(T, H, Dh))
        k = rope_f((xn @ W[p + "self_attn.k_proj.weight"].T).reshape(T, H, Dh))
        vv = (xn @ W[p + "self_attn.v_proj.weight"].T).reshape(T, H, Dh)
        sc = np.einsum("ihd,jhd->hij", q, k) / np.sqrt(Dh)
        sc = np.exp(sc - sc.max(-1, keepdims=True)); sc /= sc.sum(-1, keepdims=True)
        o = np.einsum("hij,jhd->ihd", sc, vv).reshape(T, D)
        h = h + o @ W[p + "self_attn.o_proj.weight"].T
        xn = ln(h, W[p + "post_attention_layernorm.weight"])
        f = gelu(xn @ W[p + "mlp.fc1.weight"].T + W[p + "mlp.fc1.bias"])
        h = h + f @ W[p + "mlp.fc2.weight"].T + W[p + "mlp.fc2.bias"]
        if l == 0:
            l0 = h.copy()
    enc = ln(h, W["model.encoder.layer_norm.weight"])
    return stem, l0, enc


def ref_decoder(W, cfg, enc, ids, gate_first, cross_rope):
    """numpy 参考解码器: 前缀 ids 整段过一遍, 回末位 logits。两处口径由参数定, 对 ONNX 判定: gate_first = fc1 输出前半是 gate(否则后半);
    cross_rope = 交叉注意力的 q 也转 RoPE。"""
    D, H = cfg["hidden_size"], cfg["decoder_num_attention_heads"]
    Dh, eps = D // H, 1e-5
    rot = int(Dh * cfg["partial_rotary_factor"]); theta = cfg["rope_theta"]
    E = W["model.decoder.embed_tokens.weight"]
    h = E[np.array(ids)].astype(np.float64)
    T = h.shape[0]; Te = enc.shape[0]
    inv = theta ** (-np.arange(0, rot, 2, dtype=np.float64) / rot)

    def ln(v, w): return (v - v.mean(-1, keepdims=True)) / np.sqrt(v.var(-1, keepdims=True) + eps) * w

    def rope_f(q):
        n = q.shape[0]; ang = np.arange(n, dtype=np.float64)[:, None] * inv[None, :]
        c, s = np.cos(ang)[:, None, :], np.sin(ang)[:, None, :]
        q = q.copy(); x1, x2 = q[..., 0:rot:2].copy(), q[..., 1:rot:2].copy()
        q[..., 0:rot:2] = x1 * c - x2 * s; q[..., 1:rot:2] = x2 * c + x1 * s
        return q

    def attn(q, k, v, causal):
        sc = np.einsum("ihd,jhd->hij", q, k) / np.sqrt(Dh)
        if causal:
            sc = sc + np.triu(np.full((q.shape[0], k.shape[0]), -1e30), 1)[None]
        sc = np.exp(sc - sc.max(-1, keepdims=True)); sc /= sc.sum(-1, keepdims=True)
        return np.einsum("hij,jhd->ihd", sc, v).reshape(q.shape[0], D)

    silu = lambda g: g / (1.0 + np.exp(-g))
    for l in range(cfg["decoder_num_hidden_layers"]):
        p = f"model.decoder.layers.{l}."
        xn = ln(h, W[p + "input_layernorm.weight"])
        q = rope_f((xn @ W[p + "self_attn.q_proj.weight"].T).reshape(T, H, Dh))
        k = rope_f((xn @ W[p + "self_attn.k_proj.weight"].T).reshape(T, H, Dh))
        v = (xn @ W[p + "self_attn.v_proj.weight"].T).reshape(T, H, Dh)
        h = h + attn(q, k, v, True) @ W[p + "self_attn.o_proj.weight"].T
        xn = ln(h, W[p + "post_attention_layernorm.weight"])
        q = (xn @ W[p + "encoder_attn.q_proj.weight"].T).reshape(T, H, Dh)
        k = (enc @ W[p + "encoder_attn.k_proj.weight"].T).reshape(Te, H, Dh)
        if cross_rope:
            q, k = rope_f(q), rope_f(k)
        v = (enc @ W[p + "encoder_attn.v_proj.weight"].T).reshape(Te, H, Dh)
        h = h + attn(q, k, v, False) @ W[p + "encoder_attn.o_proj.weight"].T
        xn = ln(h, W[p + "final_layernorm.weight"])
        gu = xn @ W[p + "mlp.fc1.weight"].T + W[p + "mlp.fc1.bias"]
        I = gu.shape[1] // 2
        g, u = (gu[:, :I], gu[:, I:]) if gate_first else (gu[:, I:], gu[:, :I])
        h = h + (silu(g) * u) @ W[p + "mlp.fc2.weight"].T + W[p + "mlp.fc2.bias"]
    xn = ln(h, W["model.decoder.norm.weight"])
    return xn[-1] @ E.T


def _erf(v):
    """np 没有 erf; A&S 7.1.26(与 metal/unary.metal 的 erf_approx 同式, 绝对误差 1.5e-7)"""
    sg = np.sign(v); v = np.abs(v)
    t = 1.0 / (1.0 + 0.3275911 * v)
    y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * np.exp(-v * v)
    return sg * y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--wav", required=True)
    ap.add_argument("--tok", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", help="safetensors 模型目录: 给了就用 numpy 参考判定 RoPE 配对方式并把 stem / layer0 段也写进夹具")
    a = ap.parse_args()
    x = read_wav16k(a.wav)
    enc = ort.InferenceSession(f"{a.onnx}/onnx/encoder_model.onnx", providers=["CPUExecutionProvider"])
    dec = ort.InferenceSession(f"{a.onnx}/onnx/decoder_model.onnx", providers=["CPUExecutionProvider"])
    h = enc.run(None, {"input_values": x[None, :]})[0][0]   # [n_frames][288]
    stages = None
    if a.model:
        import json
        W = load_st(f"{a.model}/model.safetensors"); cfg = json.load(open(f"{a.model}/config.json"))
        best = None
        for rope in ("half", "inter"):
            stem, l0, e = ref_encoder(W, cfg, x.astype(np.float64), rope)
            d = float(np.abs(e - h).max())
            print(f"numpy ref rope={rope}: max|Δ| vs ONNX encoder = {d:.3g} (ref max {np.abs(h).max():.3g})")
            if best is None or d < best[0]:
                best = (d, rope, stem, l0)
        print(f"=> RoPE pairing that matches ONNX: {best[1]}")
        stages = (best[2].astype(np.float32), best[3].astype(np.float32))
        # 解码器口径判定: 对 ONNX 的末位 logits 比(前缀取 bos + 金标前 4 个 id)
        pre = [1, 29871, 30919, 31076, 30214]
        ref_lg = dec.run(None, {"input_ids": np.array([pre], dtype=np.int64), "encoder_hidden_states": h[None]})[0][0, -1]
        for gf in (True, False):
            for cr in (False, True):
                lg = ref_decoder(W, cfg, h.astype(np.float64), pre, gf, cr)
                print(f"numpy ref decoder gate_first={gf} cross_rope={cr}: max|Δ| vs ONNX logits = {np.abs(lg - ref_lg).max():.3g} (argmax {lg.argmax()} vs {ref_lg.argmax()})")
    max_len = int(len(x) * 13 / 16000)
    ids = [1]
    steps = []
    while len(ids) <= max_len:
        logits = dec.run(None, {"input_ids": np.array([ids], dtype=np.int64), "encoder_hidden_states": h[None]})[0][0, -1]
        top = np.argsort(-logits)[:TOPK]
        chosen = int(top[0])
        steps.append((chosen, [(int(i), float(logits[i])) for i in top]))
        ids.append(chosen)
        if chosen == 2:
            break
    text = decode(a.tok, ids[1:])
    with open(a.out, "wb") as f:
        f.write(b"ASRG" + struct.pack("<7I", 1, len(x), h.shape[0], h.shape[1], len(steps), int(logits.shape[0]), TOPK))
        f.write(h.astype("<f4").tobytes())
        for chosen, top in steps:
            f.write(struct.pack("<i", chosen))
            for i, v in top:
                f.write(struct.pack("<if", i, v))
        tb = text.encode("utf-8")
        f.write(struct.pack("<I", len(tb)) + tb)
        if stages is not None:   # 版本 1 的尾巴: "STG" + stem[n_frames*D] + layer0[n_frames*D](numpy 参考, 只作 C 侧逐段对账, 不是 ONNX 产出)
            f.write(b"STG\0" + stages[0].astype("<f4").tobytes() + stages[1].astype("<f4").tobytes())
    print(f"samples {len(x)} frames {h.shape[0]} steps {len(steps)} ids {ids[1:]}")
    print(f"text: {text}")


if __name__ == "__main__":
    main()
