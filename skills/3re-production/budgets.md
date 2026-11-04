# Game budgets per platform (v1.0.0)

The agent's presets (`docs/agent/BUDGETS.md`, A5) — the numbers the chat puts in a spec:

| preset | triangles | texture | when |
|---|---|---|---|
| mobile_character | 5,000 | 1024 | phones, many characters on screen |
| mobile_prop | 1,500 | 1024 | phones, props |
| pc_character | 30,000 | 2048 | PC / console, main characters |
| pc_prop | 8,000 | 1024 | PC / console props, weapons |
| hero_character | 50,000 | 4096 | close-up hero characters, cinematics |

Sources (as collected for BUDGETS.md): Unity Manual (legacy) "Modeling Characters for Optimal Performance"
https://docs.unity3d.com/520/Documentation/Manual/ModelingOptimizedCharacters.html ; CryEngine 3 "Character Budgets"
https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/1310905 ; CGTrader polygon-count
guide https://help.cgtrader.com/hc/en-us/articles/360015142757 ; CGAxis polygon count guide 2026
https://cgaxis.com/polygon-count-guide-how-many-polys-do-you-really-need-in-2026/ . These are industry ranges, not
rules; a game's own budget wins when the person gives one.

Measured on our assets (M2/M3 reports): some shapes cannot keep fidelity at these budgets — the vampire needs
48,344 triangles (pc budget 30,000), the bike ~80k, a glass bowl's decimation stalls at 25,676. The agent then asks
(budget vs needed count); the person decides.
