# 3re status, hardware and roadmap

The short version is in the [README](../README.md). This page keeps every detail.

## What works today

Honest status of 3re 2.10 (alpha). "Tested" means on the maintainer's PC: Windows 11, RTX 3050 laptop GPU with
4 GB, 16 GB RAM.

| Feature | Status | Notes |
|---|---|---|
| Image to 3D with TRELLIS.2 (Q4, 512) | ✅ Works, tested on 4 GB | About 7 minutes per model on the tested GPU; NVIDIA + CUDA only |
| Texture from your picture (hybrid / auto texture, several angles) | ✅ Works | Hidden sides use TRELLIS's own texture; a hidden-side gate reports doubtful backs |
| Regions with SAM 2.1 tiny (click to select) | ✅ Works | Runs on the CPU |
| Export GLB / glTF / OBJ, presets for Godot, Unity, Roblox | ✅ Works | Checked by the Khronos glTF-Validator |
| Command line (`3re-cli`) and MCP server (`3re-mcp`) | ✅ Works | Same engine as the app |
| Updates from GitHub Releases, signed, with roll-back | 🧪 New | Tested end to end against a local test server; no public release has used it yet |
| Game-Ready Asset Agent and chat (Arabic and English) | 🧪 Experimental | Rules-based, offline; on our regression set 6 of 11 assets PASS (2.7.0) — [details](agent/README.md) |
| Smart Retopology (QuadWild) | 🧪 Experimental | Thin parts (blades) can fail; results are measured, never assumed |
| Linux (NVIDIA CUDA) | 🗺️ Planned | Platform layer first, then CI builds |
| macOS (Apple Silicon) | 🗺️ Planned | After Linux |
| Animation | 🗺️ Planned | Shown as "Coming soon" in the app |

## Hardware

| Tier | What you can do | Needs |
|---|---|---|
| Any PC | Export and retopology of your own meshes | Windows 10/11 64-bit, OpenGL 3.3 (or the software 3D view) |
| **Tested for AI** | Image to 3D, textures, regions, Agent | NVIDIA GPU with **4 GB** VRAM and a CUDA 12 driver, **16 GB RAM**, about 6 GB free disk for the models |
| Bigger NVIDIA GPUs | The same, probably faster | Not measured by us yet — [send a hardware report](https://github.com/3reusa7/3re/issues/new?template=hardware-report.yml) |
| AMD / Intel GPUs, CPU only | No Image to 3D yet | Image to 3D needs CUDA |

## First start

On the first start 3re lists what it can download (about 5 GB: GPU engine libraries, TRELLIS.2 weights, SAM 2.1), with
each size, source and licence. Download now or later from Settings. Before you press Generate you can mark the regions
to keep. [Updates and first-run downloads](UPDATES.md) has the full list.

## Roadmap

- **Now:** the 2.10 and 2.11 releases (asset quality: transform hygiene, region refinement, part separation).
- **Next:** a platform layer and the Linux (CUDA) build; CI-built GPU engine; a decision on code signing.
- **Later:** macOS on Apple Silicon (experimental), animation.

Ideas and questions go to the [issues](https://github.com/3reusa7/3re/issues).
