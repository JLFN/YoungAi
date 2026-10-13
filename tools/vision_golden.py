#!/usr/bin/env python3
"""视觉塔金标夹具(2026-10-13): 用官方 inference/{image_processor,vision}.py + BF16 原件权重, 对一张图产出
预处理结果与 aligner 输出, 给 ./ds4 --image 图 --vision-golden 金标 逐段比(src/core/core_v41_vision_gate.c)。

只做夹具, 不参与任何数值链(全仓 Python 裁决: 只许做外部采集/金标生成)。在 spark 上跑:
  ~/v41env/bin/python tools/vision_golden.py --hf ~/ds4-main/hf/DeepSeek-V4.1-Flash --image img.jpeg --out /tmp/golden.bin

文件格式(小端): "VGLD" u32 ver=1 | u32 w h n_vit_h n_vit_w n_llm_h n_llm_w pp E best_w best_h
                 u8 rgb[best_h*best_w*3](PIL pad 之后) | f32 patches[n_vit*pp] | f32 embeds[n_llm*E]
"""
import argparse, json, os, struct, sys, types
import numpy as np
import torch
from PIL import Image, ImageOps
from safetensors import safe_open


def read_golden(path):
    with open(path, "rb") as f:
        assert f.read(4) == b"VGLD"
        h = struct.unpack("<12I", f.read(48))
        ver, w, h0, vh, vw, lh, lw, pp, E, bw, bh, _ = h
        rgb = np.frombuffer(f.read(bw * bh * 3), dtype=np.uint8)
        pat = np.frombuffer(f.read(vh * vw * pp * 4), dtype="<f4").reshape(vh * vw, pp)
        emb = np.frombuffer(f.read(lh * lw * E * 4), dtype="<f4").reshape(lh * lw, E)
    return rgb, pat, emb


def compare(a, b):
    """两份金标的 aligner 输出逐行比(与 core_v41_vision_gate.c 同口径: 最小行 cos / 最大绝对差, 外加均值)"""
    _, pa, ea = read_golden(a); _, pb, eb = read_golden(b)
    assert ea.shape == eb.shape, (ea.shape, eb.shape)
    cos = (ea * eb).sum(1) / (np.linalg.norm(ea, axis=1) * np.linalg.norm(eb, axis=1) + 1e-30)
    print(f"[compare] {a} vs {b}: patch 最大差 {np.abs(pa - pb).max():.5f}; embeds {ea.shape} 行 cos 最小 {cos.min():.6f}(第 {cos.argmin()} 行) "
          f"均值 {cos.mean():.6f} 中位 {np.median(cos):.6f}; 最大绝对差 {np.abs(ea - eb).max():.5f}; |mean| {np.abs(ea).mean():.4f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hf", help="原始 HF 目录(含 config.json / inference/ / model-0000*.safetensors)")
    ap.add_argument("--image")
    ap.add_argument("--out")
    ap.add_argument("--compare", nargs=2, metavar=("A.bin", "B.bin"), help="只比两份金标的 aligner 输出, 不跑模型")
    ap.add_argument("--dtype", default="bf16", choices=["bf16", "f32"],
                    help="f32 = 官方同一份代码全程 f32 跑(没有 bf16 舍入): 拿它与 bf16 金标比, 就知道官方自己的 bf16 噪声有多大, 引擎的偏差只要在这个量级里就是对的")
    a = ap.parse_args()
    if a.compare:
        compare(*a.compare); return
    if not (a.hf and a.image and a.out):
        ap.error("--hf/--image/--out 三个都要(或者只用 --compare)")
    sys.path.insert(0, os.path.join(a.hf, "inference"))
    import image_processor as ip
    from vision import Aligner, ViT

    cfg = json.load(open(os.path.join(a.hf, "config.json")))
    vc = cfg["vision_config"]
    dim = cfg["text_config"]["hidden_size"]   # 顶层 config 是 HF 的 text_config/vision_config 两段式
    args = types.SimpleNamespace(
        dim=dim, vision_n_layers=vc["num_hidden_layers"], vision_dim=vc["hidden_size"],
        vision_n_heads=vc["num_attention_heads"], vision_inter_dim=vc["intermediate_size"], vision_patch_size=vc["patch_size"],
        vision_rope_theta=float(vc["rope_theta"]), vision_downsample_ratio=vc["downsample_ratio"], vision_max_n_token=vc["max_image_tokens"],
        vision_min_pixels=vc["min_pixels"], vision_max_wh_ratio=vc.get("max_wh_ratio"), image_token_id=cfg["image_token_id"], vision_enabled=True)

    # 预处理走官方函数; 补边后的像素按 load_image 同样的几行再算一遍(同一组操作 ⇒ 同一张图)
    patches, n_vit_h, n_vit_w, n_llm_h, n_llm_w = ip.load_image({"url": a.image}, args)
    with Image.open(a.image) as src:
        img = src.convert("RGB")
    w0, h0 = img.width, img.height
    lh, lw, best_h, best_w = ip.plan_image_grid(img.width, img.height, args)
    assert (lh, lw) == (n_llm_h, n_llm_w)
    padded = ImageOps.pad(img, (best_w, best_h), color=(127, 127, 127))
    rgb = np.asarray(padded, dtype=np.uint8)
    assert rgb.shape == (best_h, best_w, 3)

    # 权重: vision.* / aligner.* 在 model.safetensors.index.json 指到的分片里(BF16 原件)
    idx = json.load(open(os.path.join(a.hf, "model.safetensors.index.json")))["weight_map"]
    need = {k: v for k, v in idx.items() if k.startswith(("vision.", "aligner."))}
    sd = {}
    for shard in sorted(set(need.values())):
        with safe_open(os.path.join(a.hf, shard), framework="pt", device="cpu") as f:
            for k, v in need.items():
                if v == shard:
                    sd[k] = f.get_tensor(k)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    torch.set_default_device(dev)   # 官方 get_vision_cos_sin 用默认设备造频率表; 官方 generate.py 也是这么设的
    dt = torch.bfloat16 if a.dtype == "bf16" else torch.float32
    torch.set_default_dtype(dt)
    vit, al = ViT(args).to(dev), Aligner(args).to(dev)
    vit.load_state_dict({k[len("vision."):]: v.to(dt) for k, v in sd.items() if k.startswith("vision.")}, strict=True)
    al.load_state_dict({k[len("aligner."):]: v.to(dt) for k, v in sd.items() if k.startswith("aligner.")}, strict=True)
    with torch.inference_mode():
        emb = al(vit(patches.to(dev).to(dt), n_vit_h, n_vit_w), n_vit_h, n_vit_w)
    emb = emb.float().cpu().numpy()
    pat = patches.float().cpu().numpy().reshape(n_vit_h * n_vit_w, -1)
    assert emb.shape == (n_llm_h * n_llm_w, args.dim), emb.shape

    with open(a.out, "wb") as f:
        f.write(b"VGLD")
        f.write(struct.pack("<12I", 1, w0, h0, n_vit_h, n_vit_w, n_llm_h, n_llm_w, pat.shape[1], args.dim, best_w, best_h, 0))
        f.write(rgb.tobytes()); f.write(pat.astype("<f4").tobytes()); f.write(emb.astype("<f4").tobytes())
    print(f"[golden] {a.image} {w0}x{h0} → pad {best_w}x{best_h}, patch {n_vit_h}x{n_vit_w}, token {n_llm_h}x{n_llm_w} (span {n_llm_h*(n_llm_w+1)+2}); "
          f"embeds {emb.shape} |mean| {np.abs(emb).mean():.4f} → {a.out}")


if __name__ == "__main__":
    main()
