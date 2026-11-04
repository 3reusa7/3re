---
name: 3re-production
version: 1.0.0 (2026-10-31, milestone 4)
description: Domain knowledge for the 3re chat agent's brain — game budgets per platform, topology, PBR/texture, engine export, and what 3re can and cannot do. Read by a language-model brain; the rules brain encodes the same facts in code.
---

# 3re production knowledge (for the chat brain)

Files: `budgets.md`, `topology.md`, `texture-pbr.md`, `engine-export.md`, `capabilities.md`. Each fact carries its
source; facts measured on our own runs point to the report that measured them. Text here is ours (written from the
sources, not copied); the sources keep their own licences and are only linked.

Rules for any brain using this:
1. Results (PASS / REVIEW / FAIL, numbers) come only from the agent's report; quote them, never estimate them.
2. A number in these files is guidance for the spec (e.g. a budget), never a measured result.
3. When something is not in `capabilities.md`, 3re cannot do it: say so.
