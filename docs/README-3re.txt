3re — 3D creation studio
========================

Start: double-click 3re.exe in this folder. Nothing to install; the folder is
portable (copy it anywhere, keep its contents together).

What 3re does
  Image to 3D      Import a picture of one object or character. Optionally mark
                   the regions to keep (Segment), then generate a 3D model on
                   your GPU with TRELLIS.2 (512, 4 GB VRAM profile), preview it
                   and export a GLB. Every result is checked; problems are shown
                   in words, never hidden.
  Pixel Art Builder  Pixel-art drawings (Front, optional Side/Top) to validated
                   low-poly or solid models (Low-Poly Studio, Visual Hull).
  Export           GLB (and glTF/OBJ for pixel projects), re-imported and checked.
  Rigging          Low-Poly Studio characters only (Experimental).
  Coming Soon      Smart Retopology, rigging of generated meshes, Animation.

Requirements
  Windows 10/11 64-bit. Image to 3D generation: an NVIDIA GPU with 4 GB VRAM or
  more and about 3.3 GB free RAM while 3re is open. Segmentation runs on the CPU.
  Everything runs offline; no account, no upload.

AI models
  Releases from GitHub do not include the AI models. On the first start 3re
  offers to download them (about 5 GB: GPU engine libraries, TRELLIS.2 weights,
  SAM 2.1) and shows each one's size, source and licence first; Settings > AI
  models downloads or re-checks them later. Every file is checked by SHA-256.

Updates
  Settings > Updates: stable or alpha channel, a daily check that can be turned
  off (it only asks GitHub for the list of releases), and "Go back" to the
  previous version. Updates are signed and install only after you confirm.

Folder contents
  3re.exe          the application (starts app\3re-studio.exe)
  app\             program files, CLI (3re-cli.exe), MCP server (3re-mcp.exe),
                   updater (3re-update.exe), models.json (what is downloaded)
  models\          AI models (downloaded on first use, or bundled by a
                   maintainer's build), verified by checksum
  examples\        sample pictures and drawings
  LICENSE, NOTICE, LICENSES\, THIRD-PARTY-NOTICES.txt   licences of every component
  RELEASE.json     version and checksums of this release

Check the installation: 3re.exe --self-test   (prints PASS/WARN/FAIL per item)
Projects are saved in Documents\3re\Projects; settings in the 3re settings store.
