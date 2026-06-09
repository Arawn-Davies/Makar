---
title: acpi
parent: Kernel subsystems
grand_parent: Reference
---

# acpi - RSDP/FADT discovery, shutdown & reboot

**Header:** `kernel/include/kernel/acpi.h`
**Source:** `arch/i386/core/acpi.c`

`acpi_init()` scans for the RSDP and parses the FADT / DSDT. It must run **after
paging is enabled** (the ACPI tables in low physical memory are covered by the
256 MiB identity-mapped window). On success the PM1a/PM1b control-block ports and
the S5 sleep-type values are cached for `acpi_shutdown()`. Returns `1` if tables
were found and parsed, `0` otherwise.

---

## API

| Function | Role |
|---|---|
| `acpi_init()` | Find + parse the tables; cache the PM1/S5 values. Returns 1 on success. |
| `acpi_checksum(table,len)` | Verify a table's byte sum is zero (ACPI-valid). Backs the `test_acpi_checksum` ktest. |
| `acpi_shutdown()` | Power off via ACPI S5 ("soft off"). **Never returns.** |
| `acpi_reboot()` | Reset the machine. **Never returns.** |

## Fallback chains (resilient by design)

`acpi_shutdown()` tries, in order: (1) the ACPI PM1 control registers, (2) the
QEMU/Bochs I/O-port fallbacks (`0x604` / `0xB004`), (3) a `cli; hlt` spin (last
resort — the machine appears frozen).

`acpi_reboot()` tries: (1) the ACPI reset register (FADT rev ≥ 2, I/O-port
variant), (2) the PS/2 keyboard-controller CPU-reset pulse (`outb(0x64, 0xFE)`),
(3) a triple-fault (zero-limit IDT + `int $0`).

These back the `shutdown`/`poweroff`/`halt` and `reboot` shell commands and the
GUI power menu. The same 8042-reset + triple-fault tail is reused inline by the
`hwspecs` boot mode's "press a key to reboot" and by the fail-safe panic path
([debug](debug.md)).
