# Changelog

## v1.0.0 — first public release (loader build v5.17)
- Runs Zombiewood 1.5.3 (Android) on PS Vita: menus, gameplay, saves, local shop.
- Physical controls: sticks, power-ups on Square / Triangle / Circle with on-screen badges,
  START to pause, Cross for a centre tap; optional on-screen sticks.
- Game files are checked (size and checksum) before loading, with a readable error screen.
- `tools/prepare_data.py` / `.bat` build the data folder from your own APK and OBBs; the loader
  checks every unpacked file against its manifest, so an incomplete copy shows an error screen
  instead of crashing.
- Performance in busy levels: fewer hitches in fights.
  - HUD texture updates no longer copy the whole texture (16–19.5 ms → ~4.3 ms each).
  - The HUD's Flash garbage collection runs 8× less often (regular ~60 ms hitch every
    ~2.5 s → every ~20 s).
  - Sound effects are cached in RAM and preloaded, OBB reads go through a RAM block cache.
- Title ID `ZMBW00001`.
