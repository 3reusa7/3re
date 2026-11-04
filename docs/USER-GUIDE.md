# 3re user guide

Start `3re.exe`. The rail on the left holds every workspace; Ctrl+1 … Ctrl+8 follow it.

## First start

- **AI models.** 3re offers to download the GPU engine libraries, the TRELLIS.2 weights and SAM 2.1 (about 5 GB,
  each with its size, source and licence). Choose "Not now" to use only the pixel-art features; Settings → AI models
  downloads them later. Downloads resume after an interruption and every file is checked before use.
- **Updates.** If an update is available and the daily check is on, 3re shows what changed and asks before
  downloading anything ([UPDATES.md](UPDATES.md)).

## Agent

Type what you want in Arabic or English ("a game-ready version of this character for Roblox, under 30k triangles"),
attach pictures or a GLB with "+" or by dragging them in. The agent asks only for what is missing, runs, and reports
measured results; questions it cannot decide alone come back to you. Experimental — see [agent/README.md](agent/README.md).

## Home

Start a new model, reopen a recent project, and see whether this computer can generate (models installed, graphics
card checked, free memory, segmentation).

## Image to 3D — AI Generation

1. **Import** one picture of one object or character (PNG, JPG, WebP, Aseprite, PSD). The original is never changed.
2. **Segment** (optional): click a region (SAM on the CPU, or colour regions), label it, and choose which regions are
   used. Only included regions become 3D; "Skip" uses the automatic cutout.
3. **Generate**: TRELLIS.2 runs on the NVIDIA GPU (about 7 minutes on a 4 GB card). 3re checks free memory first.
4. **Preview**: the model with its texture, and the checks (PASS / REVIEW / FAIL, in words). Add pictures from other
   angles to improve the hidden sides; "There is a problem here" marks a region for a targeted fix.

Projects are saved as folders (`project.3re`) in Documents\3re\Projects (Settings → Generation).

## Image to 3D — Pixel Art Builder

Drop a front drawing (and side, top if you have them). 3re builds a solid or low-poly model that matches every
drawing exactly, shows what it had to estimate, and lets you edit depth, volume and parts. The full guide to the
pixel-art workflow is in [docs/README.md](README.md).

## Retopology (experimental)

Smart Retopology (QuadWild) turns a dense mesh into quads at a target polygon count and measures the result against
the original. Thin parts can fail; the result says so.

## Export

GLB, glTF or OBJ with presets for Godot, Unity, Roblox or generic. Exports are reloaded and validated.

## Rigging and animation

Rigging works for Low-Poly Studio characters only (experimental). Animation is not available yet.

## Settings

Generation (setup file, projects folder) · Saved models (re-texture all with the current texture engine) ·
Display (interface size, software 3D view) · Options (advanced details) · **Updates** (channel, daily check, check
now, go back to the previous version) · **AI models** (state, download, check files) · About (version, licences) ·
Diagnostics.

## Command line

- `3re.exe --self-test [--deep]` — check this 3re folder.
- `app\3re-cli.exe` — the pixel-art engine from the command line (`--help`).
- `app\3re-mcp.exe` — the same engine as an MCP server for AI assistants (stdio; see `docs/README-MCP.md`).
- `3re.exe --e2e-generate PICTURE --e2e-out SUMMARY.json` — one end-to-end generation without the UI.
