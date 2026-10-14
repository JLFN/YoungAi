---
license: mit
base_model: deepseek-ai/DeepSeek-V4.1-Flash
base_model_relation: quantized
language:
- en
- zh
tags:
- deepseek
- mixture-of-experts
- vector-quantization
- gguf
- cuda
- dgx-spark
- metal
- apple-silicon
- on-device
- agent
- personal-assistant
- fine-tuning
pipeline_tag: text-generation
---

<p align="center">
  <img src="web/logo.jpg" width="96" alt="YoungAi logo">
</p>

<h1 align="center">YoungAi</h1>

<p align="center">
  <b>A personal AI assistant that lives on your own machine.</b><br>
  Hold to talk. It remembers what you tell it, browses the web, writes Word/PDF/PowerPoint/Excel files, makes videos, keeps
  following up on a job, and hands the result to the next app on your phone. Model, memory and files all stay on the box.
</p>

<p align="center">
  <a href="https://fodelf.github.io/YoungAi/"><b>Website · user guide</b></a> ·
  <b>English</b> · <a href="https://github.com/fodelf/YoungAi/blob/restructure/README.zh-CN.md">中文</a> ·
  <a href="https://huggingface.co/wenzhouwu/YoungAi-DeepSeek-V4.1-Flash">Hugging Face</a> ·
  <a href="https://github.com/fodelf/YoungAi/blob/restructure/docs/TECHNICAL.md">Technical notes</a>
</p>

![YoungAi Studio: the assistant hunting for the cheapest DGX Spark](docs/img/studio-agent-en.png)

The model behind it is DeepSeek V4.1 Flash (510 GB as released), shrunk to 113.6 GB and running on a single 128 GB
DGX Spark with a 1M-token context. Open source, MIT. Plain C99 + CUDA (Metal on Apple Silicon), one Makefile, no
third-party libraries linked.

## What it does

**A request comes in, the assistant does the work in a sandbox, then hands the result on.** The screenshot above is a
real run: "find me the cheapest DGX Spark on the web" took 16 minutes, 12 rounds and 43 tool calls, all on the machine itself.

| You say | It does | You get |
|---|---|---|
| "Anything good to eat nearby?" (on a phone) | locate → nearby places → picks 3 with reasons | a *Navigate* button and an *Open in Meituan / Ele.me* button for each |
| "Turn this Word file into a PDF" (attachment) | converts it inside the sandbox | a file card: download, or share to WeChat / DingTalk |
| "Look into X and write it up for my colleague" | searches, reads the sources, writes a docx | the report plus the system share sheet |
| "Sum this table by month" | reads the csv, writes an xlsx with formulas | an Excel file whose numbers show on a phone |
| "Make a 30-second product video with narration" | writes an HTML animation, renders and encodes it frame by frame | a narrated mp4 any phone player can play |
| "Watch this price and tell me when it drops below X" | creates a task and decides itself when to look again | a push notification on your phone |
| "Remember I mostly live in Nanjing" | writes it to memory | trip plans start from Nanjing from then on |

The assistant's tools (this table is what the model sees; there is no scenario-specific prompt):

| Group | Tools | Notes |
|---|---|---|
| Memory | `remember` `forget` | only what you said yourself; plain text under `~/.ds4/assistant/memory/` you can edit |
| Files | `read_file` `list_dir` `search_files` `write_file` `edit_file` | the workspace by default; a path outside it shows a card asking you first |
| Programs | `run` | runs a command in the kernel sandbox: no network, writes only to the workspace; uses soffice / pandoc / python3 etc. if installed |
| Web | `web_search` `visit_page` | headless Firefox: search and page text |
| Office files | `write_document` `write_xlsx` | Markdown → docx / pdf / pptx; JSON → xlsx with formulas computed. All written in-house, no Office needed |
| Video | `make_video` | the model writes an HTML animation; our own H.264/AAC encoder turns it into an mp4, optionally narrated |
| Places | `locate` `nearby` | phone location + AMap nearby search (real places with an AMap key; a street-scoped web search without one) |
| To your phone | `share` `copy_text` `navigate` `call` `open_url` `open_app` `read_clipboard` `pick_file` | run on the device you are using; opens Douyin / WeChat / DingTalk / Alipay / Meituan / Ele.me |
| Follow-up | `task_check_in` `task_done` `notify` | a task picks its own next check; reminders and "done" notices go to your phone (Web Push) |
| Skills | `use_skill` | a folder with a `SKILL.md` teaches it how to do a kind of job (Agent Skills format, no scripts allowed) |
| Cloud phone | six `phone_*` tools | only in danger mode; see the disclaimer at the end |

### Voice input: hold to talk

Like WeChat: with the input box empty, the rightmost button is a microphone; tap it to get a "Hold to talk" bar, hold
and speak, and let go to **send right away**. Slide up before letting go to cancel. The limit is 30 seconds. Speech
recognition also runs on the machine's GPU, so the audio never leaves it.

- The recognizer is [moonshine-tiny-zh](https://huggingface.co/moonshine-ai/moonshine-tiny-zh) (27M parameters, 108 MB),
  run by this project's own C + GPU kernels (CUDA / Metal), with no Python or ONNX. On the Spark a 3.4-second utterance
  takes 51 ms to transcribe.
- **Chinese only.** It is the smallest tier: the paper reports a 29–36% character error rate on hard test sets
  (crowd-sourced recordings, many accents). Speaking clearly into a phone does much better, but it is a notch below large models.
- **To turn it on**, put the model's files in `gguf/asr/moonshine-tiny-zh/` and load the model again on *Models*; a
  microphone appears next to the input box. Without that folder the feature simply does not exist: no microphone, no error.

  ```sh
  mkdir -p gguf/asr/moonshine-tiny-zh && cd gguf/asr/moonshine-tiny-zh
  for f in config.json tokenizer.json model.safetensors LICENSE.txt; do
    curl -fLO https://huggingface.co/moonshine-ai/moonshine-tiny-zh/resolve/main/$f
  done
  ```

- Browsers only grant the microphone over https or localhost; over `http://<LAN-IP>` there is no microphone, and the
  rightmost button is just Send.
- Status: transcripts on the Mac (Metal) and the Spark (CUDA) match the official reference implementation character for
  character, and the production server on the Spark passed a smoke test. A real phone has been tried on a few sentences (two
  wrong characters in three); the formal acceptance run (10 sentences on each of three phones) has not been done yet.

## Safety: the sandbox, not trust in the model

- **Every tool call runs in a fresh child process that locks itself into a kernel sandbox first**: Landlock + seccomp on
  Linux, Seatbelt on macOS. If the sandbox cannot be applied, the call is refused. Thirty-odd escape tests pass on both the
  Mac and the Spark.
- **The gatekeeper is one deterministic C function**, not a second model acting as judge. Writes and deletes outside the
  workspace need your OK (this once / this session / always); arguments that look like passwords, card numbers or one-time
  codes are refused outright; the assistant cannot touch its own config directory, so it cannot grant itself permissions.
- **Web content is untrusted input**: after reading a page, standing approvals for outbound actions stop applying.
- **Every action is one line in an audit log** (`audit.jsonl`), browsable on the *Activity* tab, where approvals can be revoked.
- The honest limit: there is no VM. The assistant runs as your user; if the machine is compromised, so is the assistant.

## Get started

You need a DGX Spark (Linux aarch64 with the CUDA 13 runtime, ~320 GB free SSD). The web tools need Firefox, which
Ubuntu on the Spark ships.

```sh
git clone https://github.com/fodelf/YoungAi.git ~/ds4-main && cd ~/ds4-main
make cuda-spark
./ds4-train --host 0.0.0.0 --port 8000      # open http://<host>:8000/
```

1. **Models**: press *Download* (~317 GB, resumable), then *Load* (~2 minutes).
2. **Assistant**: just talk. Conversations, memory and the workspace live in `~/.ds4/assistant/`.
3. **Phone**: open the page and *Add to Home Screen* to get an app. Location, clipboard, sharing and push need https:
   enter a cloud server in *Settings* and turn on the tunnel, or use Tailscale. Steps and troubleshooting:
   [docs/pwa.md](docs/pwa.md) (Chinese).
4. **Tasks**: type one sentence to hand a job over; turn on *Receive notifications on this device* to be pinged when it is done or due.
5. **Skills**: create one, or import a folder with a `SKILL.md` (the repo ships one for slide decks: [`skills/pptx/`](skills/pptx/)).

How every tab and every tool works, and what happens when something goes wrong: the
**[user guide](https://fodelf.github.io/YoungAi/guide.html)**.

## Two more things it does

**Train on your own data.** One JSON object per line, in either of Unsloth's two shapes; drop the file on *Data*, press
*Start* on *Train*. Nothing is mounted automatically: turn on the *Post-train* switch next to the chat box to use it.
Full walk-through: [technical notes §4.3](docs/TECHNICAL.md#43-the-post-training-file).

```json
{"messages":[{"role":"user","content":"What was the entry price?"},{"role":"assistant","content":"30.39"}],"context":"…the source document…"}
{"text":"Raw text, trained as-is (continued pretraining)."}
```

**Use it as an API.** `/v1/chat/completions`, `/v1/completions`, `/v1/responses`, `/v1/messages` (OpenAI / Anthropic
compatible). Create a key on the *API* tab and send `Authorization: Bearer <key>` (also from localhost; without it you get
401). `"temperature": 0` gives byte-identical output every run. With the speech model installed there is also
`/v1/audio/transcriptions`: the request body is a 16 kHz mono WAV as-is (not OpenAI's multipart form) and the reply is
`{"text":"…"}`; other sample rates or formats get a 400 saying what arrived. API only, no Studio, no build:

```sh
curl -fsSLO https://huggingface.co/wenzhouwu/YoungAi-DeepSeek-V4.1-Flash/resolve/main/install.sh
bash install.sh                      # --domain law for another domain (default: code); --endpoint https://hf-mirror.com for a mirror
```

## The model

| | |
|---|---|
| Size | 510 GB as released → 113.6 GB base (8 weights vector-quantized together, ~1.6 bit/weight, no training data used) |
| Speed (DGX Spark) | reads 1,055 tokens/s (12.5k-token prompt); writes 45.5 tokens/s (a real 14k-token agent request, speculative decoding on) |
| Context | 1M tokens; the full KV cache is 0.89 GB |
| Domain sidecars | finance / code / law / medicine / science, ~40 MB each, switched from the chat box in under a second |

With a sidecar, how often our next token matches the original full-precision model's goes up 2.9–3.7 points (code:
78.6% → 82.3%). Method and full tables: [technical notes §5](docs/TECHNICAL.md#5-results).

**On a Mac**, `make` builds the Metal version; 126 GPU kernel tests pass on a real Apple GPU. The full model has not run on a
Mac yet (ours has 16 GB).

## Limits

- Tested end to end only on the DGX Spark; every number here comes from it.
- One machine, one model: background tasks and your chat take turns rather than running in parallel.
- Sites behind a login or a captcha stay closed to the assistant; it says what it could not open instead of guessing.
- Sidecars are scored on held-out text, not on end-to-end tasks.

## Danger mode (experimental) — disclaimer

Settings has a **Danger mode** switch, **off by default**. Turning it on starts a virtual Android 13 phone on the host
(Linux only: the kernel needs `binder`; macOS cannot run it) and gives the model six `phone_*` tools: look at the screen,
tap, swipe, press keys, type, open apps. The workbench gets a **Phone** tab with the live screen (H.264 stream decoded by
your browser), and `/api/phone/*` is a plain REST API that the page, the model and your own scripts all share
([`docs/assistant_phone.md`](docs/assistant_phone.md)). Read this before you flip the switch:

- **The model acts on its own.** With danger mode on, nobody confirms each tap. The phone starts empty (no SIM, no accounts,
  no app store), but **any account you sign into and any APK you install there, the model can use** — it may send messages,
  place orders, pay, or delete things. Whoever turns the switch on bears the consequences.
- **Screen content is untrusted input.** Text inside apps and web pages can try to instruct the model. The phone runs in its
  own namespaces with a hard memory cap and cannot reach your LAN or the host, but this is not a guarantee against every misuse.
- **It will not bypass anti-bot checks.** If an app shows a slide captcha or a risk-control screen, you do it yourself in the
  Phone tab; the model is not told to get around it. Do not use this to circumvent any app's terms of service or other
  people's rights.
- The phone system is a third-party image ([redroid](https://github.com/remote-android/redroid-doc), Apache-2.0) that this
  project's runtime (`ds4-phone`, ~600 lines of C, no Docker) boots directly; its one-time setup needs `sudo` once
  (`scripts/phone_prepare_linux.sh`).
- The live video needs a secure context (https, or the page opened as `localhost`); over plain `http://<LAN-IP>` browsers
  withhold WebCodecs and the tab falls back to one screenshot per second.

## Author · acknowledgements · license

Wenzhou Wu (吴文周) · 310066827@qq.com. Questions, reproduction reports and collaboration are welcome; bugs and pull
requests go to [GitHub](https://github.com/fodelf/YoungAi).

YoungAi started as a fork of [antirez/ds4](https://github.com/antirez/ds4) (DwarfStar, Salvatore Sanfilippo's DeepSeek V4
engine). GGUF, layouts such as q4_K and much kernel know-how come from [llama.cpp and GGML](https://github.com/ggml-org/llama.cpp);
the GGML copyright notice stays in `LICENSE`. The model is DeepSeek's; thanks for releasing the weights and the reference code.

Engine: MIT ([`LICENSE`](LICENSE)). The quantized weights derive from DeepSeek V4.1 Flash, also MIT
([`LICENSE-DeepSeek`](LICENSE-DeepSeek)).

Voice input is Powered by Moonshine AI. The moonshine-tiny-zh recognizer is **not MIT**; it is under the Moonshine AI
Community License: free for research and non-commercial use; commercial use requires registering at
moonshine.ai/community-license, the license ends automatically once you and your affiliates exceed US$1M in annual
revenue, and its outputs may not be used to train foundation models. This repository does not ship its weights; using it is your call.
