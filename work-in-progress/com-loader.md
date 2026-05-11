# COM loader — flat binary execution

> **Status:** placeholder — implementation not yet started
> **Milestone:** medium-term

## Summary

Implement a simple COM binary loader that loads flat DOS-style COM
executables at a fixed address and transfers control to them, enabling
the lowest-common-denominator binary compatibility with Medli.

## Acceptance criteria

- [ ] Load a COM binary from the FAT32 filesystem
- [ ] Map binary to the conventional base address (e.g. 0x100)
- [ ] Execute the binary in an isolated context
- [ ] Basic COM programs from Medli run without modification

## References

- Roadmap: [Makar × Medli — medium-term](docs/makar-medli.md#medium-term)
- Binary compatibility: [Makar × Medli — binary compatibility](docs/makar-medli.md#binary-compatibility-goals)
