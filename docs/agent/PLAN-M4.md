# 3re Agent — milestone 4 (conversational agent) — plan and rulings

Branch `claude/3re-milestone-4-brief-4b8e2d` (from main `dd22b14` = 2.8.0-alpha). Brief: the owner's
"Milestone 4 Engineering Brief" (target 10 November 2026). Results: `docs/reports/agent-m4/README.md`.
Workstreams are done in the brief's order; a workstream is finished completely before the next one starts.

## Rulings (made here, recorded so they can be challenged)

| # | Question | Ruling | Why |
|---|---|---|---|
| R1 | Agent first: what happens to Ctrl+1..8? | Shortcuts follow the rail as before: Ctrl+1 Agent, Ctrl+2 Home, …, Ctrl+8 Settings. Every page keeps a shortcut. | One rule ("Ctrl+N = Nth item") is easier to learn than a hole; tooltips show the key. |
| R2 | Where is "every tool call and result logged in run.jsonl" for a chat? | A conversation is a run: `Documents\3re\conversations\<name>\run.jsonl` (same line format as the agent's log: `event`, `timestamp`). Agent runs the chat starts keep their own `run.jsonl`; the conversation log names their folder. | The agent writes its log while it runs; a second writer on the same file would interleave lines. |
| R3 | Offline brain | A deterministic **rules** provider (AR + EN keyword/number grammar, no model) is always available and is the default. Language-model providers (local llama.cpp / LM Studio server, cloud) are optional and go through the same interface. | "Offline must always work"; nothing is downloaded or sent without the owner's choice. |
| R4 | Who decides PASS/REVIEW/FAIL in chat? | Only the agent's report (`report.json`). The chat shows "measured" lines verbatim from the report and labels the brain's text "explanation (model)" or "explanation (rules)". | Brief non-negotiable. |
| R5 | Unordered pictures: who proposes labels without a vision model? | In order: file name words (front/back/left/right/side/3-4, أمام/خلف/يمين/يسار/جانب), then the brain (vision model when one is configured), else "unknown → ask". Every label is then verified by the multi-angle registration on the generated mesh (worker status `accepted` / `rejected`, labelled vs best-fit yaw). | A proposal is a guess; the registration is the measurement. |
| R6 | Style words | Stored as `requested_style` in the spec and the report with "not applied (style transfer is not implemented)". Never changes any step. | Brief. |
| R7 | Rigging requests | The chat answers that 3re does not rig generated characters (Rigging page is Pixel Art Builder only). | Brief; true today. |

## W2 — hooded back picture (see `docs/agent/W2-HOODED-BACK.md`)

## W3 — conversation

* `apps/agent/chat/`: `ChatProvider` interface (`reply(conversation, tools) → text + tool calls`), providers
  `rules` (deterministic), `fake` (scripted, tests), `openai_compat` (HTTP to a local server or a cloud API).
* Tools (JSON schema, each call + result logged): `make_spec`, `validate_spec`, `propose_labels`, `run_agent`,
  `run_status`, `answer_decision`, `retexture_with_pictures`, `regional_fix` (W4), `open_report`.
* `3re-chat.exe`: the same conversation engine headless (scripted demos, tests, reproducibility).
* App: chat panel on the Agent page (messages, "+" and drag-and-drop for pictures/GLBs, Arabic/English,
  right-to-left text when Arabic), the run viewer beside it.
* Spec: `input.views: [{picture, label}]` (multi-angle texture from the start), `requested_style`.
* Skills: `skills/3re-production/*.md` (budgets, topology, PBR, export, capabilities), each with sources and licence.

## Order of work

W1 → W2 → W3 (research note first) → W4 → W5 → W6 → W7 → report, package, merge.
