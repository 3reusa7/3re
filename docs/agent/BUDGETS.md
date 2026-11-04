# Triangle and texture budget presets (agent milestone 2, A5)

A spec may name a `preset` instead of numbers; explicit `triangle_budget` / `texture_size` always win. Budgets are
triangles of LOD0 (engines swap lower LODs in at distance). The benchmark specs (`docs/agent/specs-m2/`) give their
numbers explicitly, the same as milestone 1's, fixed before any M2 run and never changed after seeing results.

| Preset | Triangles | Texture | Basis |
|---|---|---|---|
| `mobile_character` | 5,000 | 1024 | mobile hero characters are commonly budgeted 1,500–5,000 triangles (under 5,000 for 60 fps on mid-range Android) [1][2] |
| `mobile_prop` | 1,500 | 1024 | mobile props a few hundred to ~1,500 triangles [1][2]; Unity's legacy guidance: 300–1,500 polygons per mesh on mobile [3] |
| `pc_character` | 30,000 | 2048 | PC/console main characters typically 15,000–50,000 triangles; 20,000–40,000 for the body in console action games [1][4] |
| `pc_prop` | 8,000 | 1024 | a detailed hand-held/near-camera prop on PC/console; well below a character budget [1][4] |
| `hero_character` | 50,000 | 4096 | the top of the PC/console hero range, clothing and accessories included [1][4][5] |

These are starting points, not rules: scene density, frame rate, overdraw, draw calls and texture memory often
matter more than one mesh's triangles [2][3][6] — profile on the target hardware. Older engine documentation is far
lower (CryEngine 3: 9,500–11,500 triangles for a main character with attachments [7]); the presets follow current
practice, not that.

Sources (most are vendor guides and practitioner forums, not engine requirements):

1. Polygon count guide for 3D game assets — https://blog.neural4d.com/user-guide/polygon-count-for-3d-game-assets/
2. CGTrader, recommended polygon count for game and VR/AR models — https://help.cgtrader.com/hc/en-us/articles/360015142757-What-is-the-recommended-polygon-count-for-game-and-VR-AR-models
3. Unity Manual (legacy), Modeling Characters for Optimal Performance — https://docs.unity3d.com/520/Documentation/Manual/ModelingOptimizedCharacters.html
4. CGAxis, polygon count guide 2026 — https://cgaxis.com/polygon-count-guide-how-many-polys-do-you-really-need-in-2026/
5. Unity Discussions, "Are 40,000 tris too much for a desktop game model?" — https://discussions.unity.com/t/are-40-000-tris-too-much-for-a-desktop-game-model/572425
6. Unity Discussions, "How many polygons is too much?" — https://discussions.unity.com/t/how-many-polygons-is-too-much/4454
7. CryEngine 3 documentation, Character Budgets — https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/1310905
