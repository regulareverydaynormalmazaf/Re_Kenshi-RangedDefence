# Ranged Defence

A **RE_Kenshi / KenshiLib** plugin for **Kenshi**. Gives every character — yours and NPCs
alike — a real chance to **dodge or block incoming ranged attacks** (turrets, crossbows),
rolled with the game's own combat formulas against the shooter's skill (not a flat 100%).
Fully configurable via the in‑game **Mod Hub** or `mod-config.json`.

**Download, screenshots and full description:**
https://www.nexusmods.com/kenshi/mods/2135?tab=description

Fully supported on **Steam** and **GOG** Kenshi **1.0.65**

## Requirements (players)
- [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) — **required**.
- [Emkejs-Mod-Core](https://www.nexusmods.com/kenshi/mods/1885) — optional; enables the in‑game
  Mod Hub settings panel. Without it the mod still works and reads `mod-config.json`.

Recommended load order: **Emkejs-Mod-Core**, then **this mod**.

## Building
- Visual Studio, Platform Toolset **v100**, configuration **x64 / Release**, Character Set: **Unicode**.
- **External build dependencies (not included in this repo):**
  - **KenshiLib** SDK — headers `kenshi/*` and `kenshilib.lib` (from RE_Kenshi / KenshiLib), 
    get it here: https://github.com/BFrizzleFoShizzle/RE_Kenshi.
  - **`mod_hub_api.h`** — the Emkejs‑Mod‑Core SDK header. Get it here:
    https://github.com/Emkej/Emkejs-Mod-Core/tree/main/include/emc
    and place `mod_hub_api.h` in `RangedDefence/` next to `RangedDefence.cpp` before building.
- The build writes `RangedDefence.dll` into `mod/RangedDefence/` (project `OutDir`).
- Note: MSVC v100 has no `/utf-8`, so all Russian UI strings are stored as `\xHH` byte escapes
  on purpose — do not "fix" them to readable Cyrillic.

## Installing
`mod/RangedDefence/` is the installable mod folder. After building, `RangedDefence.dll` sits in
it next to `RE_Kenshi.json`, `RangedDefence.mod` and `mod-config.json`. Copy the whole
`mod/RangedDefence/` folder into your Kenshi `mods/` directory, then enable it in the launcher.

## License
**GPLv3** — see [LICENSE](LICENSE). This mod links against and builds on GPLv3 works
(RE_Kenshi / KenshiLib), so it is distributed under the same license.

RangedDefence — ranged dodge/block for Kenshi (RE_Kenshi / KenshiLib plugin)

Copyright (C) 2026 regulareverydaynormalmazaf

SPDX-License-Identifier: GPL-3.0-or-later

This program is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
Distributed WITHOUT ANY WARRANTY. See the GNU General Public License for details.
You should have received a copy of the GNU GPL along with this program.
If not, see <https://www.gnu.org/licenses/>.

## Credits
- **BFrizzleFoShizzle** — RE_Kenshi & KenshiLib
- **Emkej** — Emkejs-Mod-Core
