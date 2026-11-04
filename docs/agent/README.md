# Game-Ready Asset Agent (experimental)

The agent turns a request (pictures or a GLB, a platform, a triangle budget) into a game-ready asset by planning
steps, running 3re's own tools, measuring each result against the spec's gates and asking you when it cannot decide.
It runs offline; its "brain" is a deterministic rules brain (Arabic and English) — a local language model was measured
and not adopted ([research](../research/agent-chat-brain.md)).

| Document | What it covers |
|---|---|
| [PLAN.md](PLAN.md) | Milestone 1: spec, planner, recovery, executor, acceptance |
| [PLAN-M2.md](PLAN-M2.md), [PLAN-M4.md](PLAN-M4.md) | Later milestones: texture candidates, chat and regional fixes |
| [BUDGETS.md](BUDGETS.md) | Triangle budgets per platform |
| [FIDELITY.md](FIDELITY.md) | How fidelity to the picture is measured |
| [TEXTURE-CANDIDATES.md](TEXTURE-CANDIDATES.md) | Texture candidates: several textures of one mesh, the best one kept |

Programs: `3re-agent.exe` (one run from a spec), `3re-chat.exe` (the conversation; `--script`, `--eval`),
`pixel3d-regional-fix.exe` ("there is a problem here"). Code: `apps/agent/`; tests: `tests/agent/`.

Measured state (maintainer's regression set, 2.7.0): 6 of 11 assets PASS; the rest fail with a stated reason. The
regression pictures are not published (several are third-party); the specs that referred to them are not part of the
public repository.
