# FreeDOOM (game data)

**Files:** `wads/freedoom1.wad`, `wads/freedoom2.wad` — fetched at build time by
[`getfreedoom.sh`](../getfreedoom.sh) into the gitignored `wads/` directory and
staged onto the ISO at `/apps/`.  **Not committed to this repository.**

## License / Origin

FreeDOOM is a project to create a complete, free-content IWAD for the DOOM
engine.  Its game data (the `freedoom1.wad` / `freedoom2.wad` IWADs) is
distributed under the **BSD 3-Clause License**.

- Project: https://freedoom.github.io/
- Source / releases: https://github.com/freedoom/freedoom
- License text: https://github.com/freedoom/freedoom/blob/master/COPYING.adoc

```
Copyright (c) 2001-2024 Contributors to the Freedoom project.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
  3. Neither the names of the copyright holders nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE.
```

## Why it's here

FreeDOOM lets Makar ship a **playable DOOM out of the box** without
distributing id Software's copyrighted `DOOM.WAD`.  The DOOM *engine* code
(doomgeneric / id's GPLv2 source) is a separate program, `doom.elf`; FreeDOOM
provides only the freely-licensed game *assets* it loads.  `getfreedoom.sh`
downloads it on demand; `doomgeneric_makar.c` prefers `freedoom1.wad` over any
`DOOM.WAD`.
