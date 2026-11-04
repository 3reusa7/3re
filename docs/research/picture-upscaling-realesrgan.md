# Research: Real-ESRGAN upscaling of the picture before projection (agent M2, B3) — negative result

Status: **not adopted** (documented negative result; nothing merged into the pipeline).

## 1. Problem

Picture-owned texels look soft in close-ups (faces, hands) — `docs/agent/TEXTURE-DIAGNOSIS.md` D4, D8, D12.

## 2. Study

Real-ESRGAN (Wang et al., ICCVW 2021; https://github.com/xinntao/Real-ESRGAN) with the ncnn-vulkan port
(https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan): `realesrgan-x4plus` (photos/renders), `realesrgan-x4plus-anime`,
`realesr-animevideov3` (drawn/cel). Fact (measured): at 2048 the atlas, not the picture, limits most of the surface
(the anime-style character needs ~7.3 M texels at picture density); only regions given extra density (faces, hands: ×2) get more texels
than picture pixels, so upscaling can only help there.

## 3. Feasibility (measured, RTX 3050)

Release realesrgan-ncnn-vulkan-20220424-windows.zip, 45,474,481 bytes, SHA-256 abc02804…802d. anime x4 14–19 s /
~1.1 GiB VRAM, animevideov3 x4 7 s, x4plus ~40 s / ~1.3 GiB. Colours preserved (mean ΔE 0.010–0.016 at original size).
The texture worker accepts ≤ 16.7 Mpx / 4096 px, so x4 results were Lanczos-reduced to x2.

## 4. Licence

Real-ESRGAN BSD-3-Clause, ncnn-vulkan port MIT, ncnn BSD-3-Clause; the zip's `vcomp140d.dll` (Microsoft debug
runtime) is not redistributable — it would have to be left out.

## 5. Criteria (the brief's, before running) and result

"Keep it only if close-ups visibly improve without halos or invented artefacts; pixel art never AI-upscaled."
Measured close-up gradient energy +4 % to +24 % (rogue, vampire, the anime-style character), but by eye: the anime-style character — orange/pink halos on neck
and fist, knuckle lines lost; rogue — pale halo on a fingertip, leather smoother; vampire — hair crisper, no halo.
→ **criteria not met** (2 of 3). Images: `docs/reports/agent-m2/b3/`.

## 6. If revisited

Upscale only the face/hand crops with a halo check (edge overshoot against the original), or a larger atlas for hero
characters (the 4096 path exists) — not studied further here.
