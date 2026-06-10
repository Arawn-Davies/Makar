# Third-party licences

Makar itself is **MIT** (© 2026 Arawn Davies) — see [`../LICENSE`](../LICENSE).
Everything Arawn wrote (the kernel, the shells, mxweb's HTML/CSS engine, the
from-scratch PNG/JPEG/BMP decoders, mxfiles/mxedit/mxterm/mxcalc/… and the rest)
is covered by that.

The components below are **external** and keep their own licences. Each file
here begins with an "Applies to:" header naming the **source files / paths** it
covers, followed by the upstream licence text. The per-application **Help → About**
dialogs surface the same attributions.

| Licence file | Component | Used by | Licence |
|---|---|---|---|
| [BearSSL.txt](BearSSL.txt) | TLS 1.2 client | `mxweb`, `wget` (HTTPS) | MIT (Thomas Pornin) |
| [lwIP.txt](lwIP.txt) | TCP/IP stack + DNS/DHCP | kernel `net/` | BSD-3 (SICS) |
| [doomgeneric.txt](doomgeneric.txt) | Doom engine | `mxdoom` | GPLv2 (ozkl, ex-id Software) |
| [FreeDoom.txt](FreeDoom.txt) | Doom game *assets* (WADs) | `mxdoom` data | **BSD — not GNU** (Freedoom project) |
| [TinyCC.txt](TinyCC.txt) | in-OS C compiler | `tcc.elf` | LGPL-2.1 (Fabrice Bellard) |
| [Limine.txt](Limine.txt) | installed-disk bootloader | HDD boot | BSD-2 (mintsuki) |
| [musl.txt](musl.txt) | host cross-toolchain | build only (not shipped) | MIT (Rich Felker) |

Note: the **Doom engine** (doomgeneric, from id Software's GPL source release) is
GPL, but the **Freedoom assets** that ship as the default WADs are BSD-licensed —
two different licences for the two halves of "Doom" on Makar.
