# Configuring `engines.csv`

Each row is a slot, 1 to 24. T2/T3 step through the slots in row order.

- **Edit only the Engine column.** Don't change Slot or Glyph, and don't move whole rows.
- **Add an engine:** type its name into an empty Engine cell.
- **Remove an engine:** clear its Engine cell. Empty slots are skipped.
- **Reorder:** move names between Engine cells.
- Each engine can be used once. The device starts on the first filled slot.
- Save as UTF-8. If the glyphs turn into `?`, write them as `#` (full), `+` (half), `.` (off).
- Rebuild the firmware. If the file is wrong, the build names the line.

## Engine names

2-op FM · 6-op FM A · 6-op FM B · 6-op FM C · additive · bass drum · chiptune · chords ·
filtered noise · grain / formant · hi-hat · modal · particle · phase distortion · snare drum ·
speech · string · string machine · swarm · VA + VCF · virtual analog · wave terrain ·
waveshaping · wavetable
