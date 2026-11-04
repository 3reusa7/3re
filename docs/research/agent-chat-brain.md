# Research: the chat agent's "brain" (local model vs cloud model)

Milestone 4, W3. CLAUDE.md study rule. Written 2026-10-31 **before** any model is downloaded or called.
Status: experiment run 2026-11-01 (local Qwen3-VL-4B): **negative — not adopted** (§7). Cloud not tested (no key).

## 1. Problem

3re's agent is deterministic and is driven by a JSON spec (`docs/agent/PLAN.md`). People do not write specs; they
say "make this character game-ready for mobile, under 10k triangles" in Arabic or English and drop pictures in any
order. Something has to turn words and pictures into a spec, ask only for what is missing, and call the agent's
tools. That "brain" must never decide acceptance (PASS/REVIEW/FAIL come from the agent's own checks), must work
offline, and must not compete with TRELLIS for the 4 GB GPU or the 16 GB of RAM.

Evidence from our machine (measured 2026-10-31): free RAM with 3re, ChatGPT and Claude open ≈ 1.4–3.3 GB; the
TRELLIS wizard refuses to start below 3.25 GiB free; texture replays peak at ~3.7 GB (M3 report). GPU: RTX 3050
Laptop 4,096 MiB, 285 MiB used at idle.

## 2. Study

### (a) Local instruct + vision model

| Model (GGUF) | Licence | Weights Q4_K_M | Vision projector | Source |
|---|---|---|---|---|
| Qwen3-VL-4B-Instruct | Apache-2.0 | 2.5 GB | 454 MB (Q8_0) / 836 MB (F16) | https://huggingface.co/Qwen/Qwen3-VL-4B-Instruct-GGUF |
| Qwen3-VL-2B-Instruct | Apache-2.0 | 1.11 GB | 445 MB (Q8_0) / 819 MB (F16) | https://huggingface.co/Qwen/Qwen3-VL-2B-Instruct-GGUF |
| Gemma 3 4B it | Gemma Terms of Use (commercial use allowed under Google's prohibited-use policy; not OSI) | 2.49 GB | 851 MB (F16) | https://huggingface.co/ggml-org/gemma-3-4b-it-GGUF, https://ai.google.dev/gemma/terms |

Runtime: llama.cpp `llama-server` (MIT) serves an OpenAI-compatible `/v1/chat/completions` with images through
libmtmd (`--mmproj`) and tool calls with `--jinja` (llama.cpp `docs/multimodal.md`, `docs/function-calling.md`;
Qwen3-VL supported in llama.cpp GGUF since ~30 Oct 2025, release b6907 or later, per Unsloth's guide
https://unsloth.ai/docs/models/tutorials/qwen3-how-to-run-and-fine-tune/qwen3-vl-how-to-run-and-fine-tune).
LM Studio (already installed on this laptop, `lms.exe`; no model downloaded) exposes the same API.

Facts vs assumptions:
* Fact (sources above): sizes, licences, API shape.
* Assumption, to be measured: CPU-only latency on this laptop. A third-party page estimates ~3.1 GB VRAM for
  Qwen3-VL-4B Q4 weights alone (llmrun.dev) — the GPU is reserved for TRELLIS, so the plan is **CPU only**
  (`--n-gpu-layers 0`). Expected order of magnitude for a 4B Q4 model on a laptop CPU: a few tokens/s; a spec
  answer (~150 tokens) would then take tens of seconds; an image adds a vision-encoder pass (Qwen3-VL images are
  resized; cost grows with resolution — use 448–672 px).
* Assumption: tool-call + image in one request works with `--jinja` for Qwen3-VL (one third-party note warns that
  templates may drop image parts on tool turns). Must be tested; fallback: ask for labels in a separate, tool-less
  request.

### (b) Optional cloud model (Settings → Brain → Cloud)

Anthropic Messages API (`POST https://api.anthropic.com/v1/messages`, `anthropic-version: 2026-10-21`), images as
base64 content blocks, client tools with `strict: true`. Prices (Anthropic, cached 2026-10-06): Claude Opus 5.5
$4 / $20 per million input / output tokens; Claude Sonnet 5.5 $2 / $10; Claude Haiku 5.5 $0.10 / $0.50.

Typical task (estimate, to be replaced by the API's `usage` numbers): system prompt + skills + tool schemas ≈ 6k
tokens (cached after the first turn), 4 pictures at ~700 px ≈ 4 × ~650 tokens, 6 turns × ~300 output tokens →
≈ 15k input + 2k output per conversation ≈ **$0.10 (Opus 5.5)**, $0.05 (Sonnet 5.5), $0.003 (Haiku 5.5).
Privacy: pictures and words leave the machine → off by default, a visible "cloud" badge on every message, key in
Windows Credential Manager (never in files, never logged). Offline: the request fails → the chat says so and falls
back to the rules brain.

### (c) Rules brain (always available)

A deterministic parser (numbers, platform words, asset types, sides, style words in AR and EN) fills the spec and asks
for each missing required field. No model, no download, same tool calls. It cannot read pictures: side labels come
from file names, otherwise it asks; registration then verifies.

## 3. Feasibility on our hardware

| | RAM | VRAM | Disk | Time | Training |
|---|---|---|---|---|---|
| Qwen3-VL-4B Q4 + mmproj Q8, CPU | ~3.3 GB + context (assumed ~0.5 GB at 8k) | 0 | 3.0 GB | to measure | none |
| Qwen3-VL-2B Q4 + mmproj Q8, CPU | ~1.6 GB + context | 0 | 1.6 GB | to measure | none |
| Cloud | ~0 | 0 | 0 | network-bound, a few seconds per turn | none |
| Rules | ~0 | 0 | 0 | < 10 ms | none |

Consequence for scheduling (W5): a 4B local model cannot stay loaded during generation or texture replays on a
16 GB laptop with the user's apps open (free RAM 1.4–3.3 GB). The brain process is stopped before every heavy step
and restarted after; the chat shows "brain paused: generation needs the memory".

## 4. Licence

Qwen3-VL 2B/4B: Apache-2.0 → allowed. Gemma 3: Gemma Terms (commercial use permitted with use restrictions) → allowed
but second choice. llama.cpp: MIT. LM Studio: proprietary app, not shipped by us (user's own install) → only as a
user-chosen server URL. Cloud: commercial API, owner's account and key.

## 5. Hypothesis and pre-registered evaluation (written before any run)

H1: Qwen3-VL-4B on CPU turns written requests into correct specs at least as often as the rules brain, and labels
unordered pictures (front/back/left/right/three-quarter) better than file names alone.

Sets (committed to `docs/agent/chat-eval/` before the first run, never changed after):
* 30 written requests (15 AR, 15 EN) with the expected spec fields (asset type, preset/budget, engine, scale,
  style, pictures). Score: exact match per field; a request counts correct only if every field matches or the
  brain asked about exactly the missing field.
* 10 unordered multi-angle sets (pictures renamed to random names). Score: label accuracy **before** verification.
* 10 multi-turn conversations with expected tool-call sequences. Score: tool name + arguments correct, no tool
  called that changes anything the user did not ask for.

Acceptance to adopt a model brain as the default: request→spec ≥ 90 %, labels ≥ 80 %, tool calls ≥ 90 %, median
latency per turn ≤ 20 s on this laptop (local) and never on the GPU together with TRELLIS. Baseline: the rules brain on
the same sets.

## 6. Risks and time cost

* Latency on CPU may be too slow for a pleasant chat → keep the rules brain as default, the model as "smarter, slower".
* Template/tool-call quirks in llama.cpp for vision models → test early; fallback: two-step requests.
* RAM contention (above) → strict unload before heavy steps.
* Download (~3 GB) needs the owner's go-ahead; the cloud path needs the owner's key and budget.
Time-box: study 1 day (this note); experiment ½ day once the download is approved.

## 7. Experiment and result (2026-11-01) — negative: not adopted

Approved by the owner on 2026-11-01 ("Local Qwen3-VL-4B"). Pre-registered before any model output:
`docs/agent/chat-eval/README.md`, sets 1–3 (commits `e0577e3`, `5c8a451`). Same conversation engine and tools for both
brains (`3re-chat --eval-brain`, tools in recording mode). Raw results: `docs/agent/chat-eval/results-*.json|txt`.

Setup (measured): llama.cpp `llama-server` b12024 CPU x64 (zip SHA-256 `a8a1bc01…`), Qwen3-VL-4B-Instruct Q4_K_M
(2,497,281,664 bytes, SHA-256 `66358cb1…`) + mmproj Q8_0 (453,974,304 bytes, `30ba2c7d…`), `--jinja -c 4096 -t 8`,
temperature 0, seed 42, i5-12500H, nothing on the GPU (177 MiB used = the desktop). Pictures sent at 448 px.

| measure | rules brain | Qwen3-VL-4B (CPU) | acceptance |
|---|---|---|---|
| load | — | 9–11 s to ready | — |
| memory | ~0 | private **3.46 GB**, working set 5.1 GB (4k context); with 8k context 5.7 GB — free RAM fell from 6.0 GB to 0.8 GB | (not to share RAM with generation) |
| speed | — | prompt **23.6 tok/s**, generation **8.6 tok/s** | — |
| set 1: request → spec (30) | **29 / 30** | **15 / 30** (EN 8/15, AR 7/15) | ≥ 90 % |
| set 2: labels of unordered pictures (3 sets, 10 pictures) | 0 labelled (cannot see) | **0 labelled** — it called propose_labels with empty labels, then replied "I need a picture" although 3–4 were attached | ≥ 80 % |
| set 3: multi-turn tool use (10) | **10 / 10** | **1 / 10** (5 / 10 if a missing `propose_labels` with a single picture were forgiven — post-hoc, not the registered rule) | ≥ 90 % |
| latency per turn | median 0.016 s | median **49.8 s**, max 241 s (set 1: median 90.8 s per request, ~160 s when it runs the whole tool chain) | median ≤ 20 s |

Every acceptance criterion failed. Not adopted; the provider stays as an experimental option of `3re-chat`
(`--brain http:127.0.0.1:PORT:MODEL`), not in the app.

What the model got wrong (from the logs, not interpretation):
* Platform words: "console", "desktop", "steam", "للكونسول" → `platform: "mobile"`; "wizard" → `prop`.
* Arabic: asked again for fields that were given (3 requests started nothing); dropped Arabic style words or
  translated them ("لو بولي" → "lo poly", which the registered scoring counts as a miss).
* Did not ask where it should (Unity request started a spec; rigging requests got no question).
* **Tried to answer the agent's question for the person** twice (on the run event, before the person said anything);
  both attempts were refused only by accident (wrong question id; a picture that does not exist). → A code-level guard
  now refuses `answer_decision` in any turn started by the agent (`Session::event`), with a test (`test_chat.cpp`).
* Pictures: no side labels at all on the unordered sets.

Biases to keep in mind: the rules brain and the sets were written by the same author (its scores are optimistic);
the system prompt for the model was written once and not tuned on the sets (tuning on them would have been
hill-climbing on the test). A larger model or a GPU would change speed and possibly accuracy, but the GPU is reserved
for TRELLIS and 7B+ models do not fit next to it in 16 GB with the owner's apps.

Recommendation: keep the rules brain as the default; spend the next effort on (a) a picture-side classifier that is
measured on its own (front/back/left/right from silhouettes and the existing registration, which already finds the
angle), and (b) more words in the rules (Steam, etc.) through the same pre-registered sets. Re-test a model brain only
with a cloud provider (owner's key) or when a much faster local runtime exists; the evaluation runner is ready.
Files downloaded for the experiment (≈3 GB, `the maintainer's private evaluation folder\llm\`) can be deleted when no longer needed.
