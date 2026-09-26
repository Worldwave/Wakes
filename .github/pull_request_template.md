### Description

<!-- What this changes and why. For a bug fix, the cause, not only the symptom. -->


### Related Issues

<!-- "Fixes #123" closes the issue when this merges. Delete this section if there is none. -->


### Testing

<!-- CI builds both configs, runs the host suites and checks the image links at 0x20000
     inside 0x20000-0xFF000 with the docs/SAFETY.md settings intact. Nothing below repeats it. -->

- [ ] Host tests extended for the new behaviour, or not applicable
- [ ] Flashed and played on hardware
      <!-- Say which build (CI artifact or local) and what you exercised. If not flashed, say so. -->

### Safety

<!-- Required whenever this touches power, sleep, the watchdog, fault handling, the board
     defconfig / devicetree or flash partitions. Otherwise delete this section. -->

- [ ] Read [`docs/SAFETY.md`](https://github.com/Worldwave/Wakes/blob/main/docs/SAFETY.md) against this change
- [ ] Power-off still works (tested on hardware, every flash)
- [ ] Holding `••` for 30 s still powers the device off
- [ ] Track 1 + Track 4 while plugging in still enters the bootloader

### Version

<!-- Only for a PR that will be tagged as a release: firmware/VERSION must match the tag,
     or release.yml refuses it. -->

- [ ] `firmware/VERSION` bumped, or not needed
