# User accounts — root and guest

> **Status:** placeholder — implementation not yet started
> **Milestone:** long-term

## Summary

Implement a user account system matching the Medli account format:
`root` and `guest` accounts stored in `System\Data\usrinfo.sys`
on the FAT32 volume, with a login prompt at boot.

## Acceptance criteria

- [ ] Login prompt displayed before the shell
- [ ] `root` and `guest` accounts loaded from `System\Data\usrinfo.sys`
- [ ] File format compatible with the Medli account file
- [ ] User session context tracked (current user, privilege level)

## References

- Roadmap: [Makar × Medli — long-term](docs/makar-medli.md#long-term)
- UX conventions: [Makar × Medli — shared UX conventions](docs/makar-medli.md#shared-ux-conventions)
