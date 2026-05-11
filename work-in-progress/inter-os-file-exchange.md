# Inter-OS file exchange — shared config and data formats

> **Status:** placeholder — implementation not yet started
> **Milestone:** long-term

## Summary

Define and adopt shared plain-text (INI-style) config and data file
formats readable by both Makar and Medli from a shared FAT32 partition,
so configuration and data created on one OS can be used on the other.

## Acceptance criteria

- [ ] Agreed INI-style format spec (section headers, key=value, comments)
- [ ] Makar reads and writes config files in the agreed format
- [ ] Medli reads the same files without modification
- [ ] At least one real config file (e.g. user preferences) uses the format
- [ ] Format documented in `docs/makar-medli.md`

## References

- Roadmap: [Makar × Medli — long-term](docs/makar-medli.md#long-term)
