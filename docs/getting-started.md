---
title: Getting started
nav_order: 2
has_children: true
---

# Getting started

Use this section to get a local Makar checkout building, booting, and testing.

| Page | What it covers |
|---|---|
| [Building and running](building.md) | Host dependencies, `run.sh` command grammar, headless and GUI modes, test targets, build products, and troubleshooting. |
| [WSL2](wsl2.md) | Notes for building and running under Windows Subsystem for Linux. |

The shortest useful path on a Linux host is:

```sh
./run.sh iso build
./run.sh iso boot
./run.sh ktest
./run.sh kbtest
./run.sh gui all-tests
```

`./run.sh` with no arguments prints the same command surface as
[Building and running](building.md), including `kbtest` and `kbtest gui`.
