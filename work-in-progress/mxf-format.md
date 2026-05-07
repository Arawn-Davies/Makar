# MXF executable format — joint Makar/Medli binary format

> **Status:** placeholder — implementation not yet started
> **Milestone:** long-term

## Summary

Design and implement MXF (Makar/Medli Executable Format): a lightweight
ELF-inspired format with a common header that both kernels can parse,
containing native i686 code and supporting relocation.

## Acceptance criteria

- [ ] Joint specification agreed upon with the Medli project
- [ ] Common header parseable by both Makar and Medli
- [ ] Native i686 code section with basic relocation support
- [ ] Makar kernel loads and executes an MXF binary
- [ ] Medli can at minimum identify MXF binaries (full execution optional)

## References

- Roadmap: [Makar × Medli — long-term](docs/makar-medli.md#long-term)
- Binary compatibility: [Makar × Medli — binary compatibility](docs/makar-medli.md#binary-compatibility-goals)
