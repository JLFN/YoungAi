#!/usr/bin/env python3
"""金标夹具生成器(不参与任何数值链, 只跑一次把官方 piper 的输出落盘): 给定音素串 → 官方 piper(onnxruntime)的 id 序列 + 噪声全 0 的决定论波形。
C 实现(src/tts)拿同一串 id 跑, 逐样本和这里的 .f32 比 —— 这是 VITS 重写正确与否的唯一判决。
用法: ~/hfenv/bin/python tests/tts/gen_fixture.py zh gguf/tts/piper-dl/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx tests/tts/fixtures
      ~/hfenv/bin/python tests/tts/gen_fixture.py en gguf/tts/piper-dl/en/en_US/lessac/medium/en_US-lessac-medium.onnx tests/tts/fixtures
中文音素串 = spark 上 libespeak-ng(cmn-latn-pinyin)对测试句吐的 IPA(tests/video/tts_probe_linux.c -ipa), 按训练期 piper-phonemize 的规矩:
逗号类后面补 ", "、句末补标点、逐 Unicode 码点一个音素。英文直接用 piper 自带 espeak 的音素化(打印出来, 和 spark 的 -ipa 对一眼)。
产物: <tag>.cases(用例名列表) + <用例>.ids + <用例>.f32。"""
import sys, numpy as np
from piper import PiperVoice, SynthesisConfig
from piper.phoneme_ids import phonemes_to_ids

CASES = {
    "zh": {
        "hello": "nˈi2 χˈɑu2.",
        "t1a": "nˈi2 χˈɑu2, wˈo2 mˈə1n tɕˈi5n thˈiɛ5n pˈɑ2 mˈoɜ ɕˈiɜŋ lˈiɑ5ŋ χwˈɑ5 tˈɑu5 sˈi̪5 pˈi2 thˈə5.",
        "t1b": "sˈu5 tˈu5 thˈiɜ s.ˈə5ŋ tˈɑu5 mˈei2 mjˈɑu2 sˈi̪5 s.ˈi.ɜ wˈu2 kˈo-1 tshˈi̪ɜ!",
    },
    "en": {   # 文本, 由 piper 自带 espeak 音素化
        "en_hello": "Hello, we quantized the model to four bits today.",
        "en_speed": "Speed went up to forty-five tokens per second!",
    },
}

def main():
    tag, onnx, out = sys.argv[1], sys.argv[2], sys.argv[3]
    voice = PiperVoice.load(onnx)
    cfg = SynthesisConfig(noise_scale=0.0, noise_w_scale=0.0, length_scale=1.0, normalize_audio=False)
    names = []
    for name, src in CASES[tag].items():
        if tag == "zh":
            phonemes = list(src)
        else:
            sentences = voice.phonemize(src)
            assert len(sentences) == 1, sentences
            phonemes = sentences[0]
            print(name, "phonemes:", "".join(phonemes))
        ids = phonemes_to_ids(phonemes, voice.config.phoneme_id_map)
        audio = voice.phoneme_ids_to_audio(ids, cfg).astype(np.float32)
        with open(f"{out}/{name}.ids", "w") as f: f.write(" ".join(map(str, ids)) + "\n")
        audio.tofile(f"{out}/{name}.f32")
        names.append(name)
        print(name, "ids", len(ids), "samples", audio.shape[0], "peak %.4f" % float(np.abs(audio).max()))
    with open(f"{out}/{tag}.cases", "w") as f: f.write("\n".join(names) + "\n")

if __name__ == "__main__":
    main()
