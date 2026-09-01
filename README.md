# 🥧 PROJECT PAPYRUS (PP)

> ### DELTARUNE Chapter 1. On a 2DS. YES, IT DOES.
>
> **THEY SAID IT COULDN'T RUN. THEY SAID THE HARDWARE WAS TRASH. THEY SAID [[Buy. Sell. Kromer.]] AND MOVED ON.**
>
> **WE PUT DELTARUNE ON A LITTLE DUAL-SCREEN HANDHELD FROM 2011 AND IT. JUST. WORKS.**

---

## WHAT THIS IS

A from-scratch-mentality GameMaker Studio 1 bytecode runner port that executes **DELTARUNE Chapter 1** on the **Nintendo 2DS / old 3DS** — 240p dual screens, real buttons, no touch UI, no launcher fluff. Boot the Homebrew Launcher, tap PROJECT PAPYRUS, play.

- ✅ BC17 (GameMaker 2.x bytecode) interpreter running Deltarune ch1 on old3DS-class hardware
- ✅ Custom CTR audio stack: native DSP ADPCM streaming for music, packed PCM16 SFX bank
- ✅ Custom CTR renderer: tiled texture atlas, ETC1-style compression lane, 60fps draw batches
- ✅ Forensic breadcrumb logger — the build tells you exactly where it dies, in RAM ring + SD journal
- ✅ Save compatibility with `filech1_9`

**This is not a port of a port of a port.** The platform layer — audio, renderer, filesystem, VM hosting — was rebuilt for this hardware by hand, from the void, one register at a time. Every crash was met, read, and buried.

## INSTALL (2 minutes)

1. Copy the whole `3ds/` folder from a release to the **root** of your 2DS SD card.
2. Supply your own **legally obtained** Steam copy of DELTARUNE Chapter 1 data:
   - `SD:/3ds/papyrus/data.win` (from your Steam `DELTARUNE/data/` folder)
   - audio + gfx are preprocessed by the included `n3ds-preprocess` host tool from YOUR files
3. Hold **START** on boot → Homebrew Launcher → **Project Papyrus**.
4. Play. Chapter Select is room 0. The hallway door works. Believe it.

## BUILDING

Requirements: devkitARM, libctru, cmake, makerom.

```bash
arm-none-eabi-cmake -S . -B build/n3ds -DPLATFORM=n3ds -DCMAKE_BUILD_TYPE=Release
cmake --build build/n3ds
# => build/n3ds/papyrus.3dsx
```

Host preprocessor (converts YOUR game files to CTR textures/audio):

```bash
cmake -S tools/n3ds-preprocess -B build/preproc -DCMAKE_BUILD_TYPE=Release
cmake --build build/preproc
```

## STATUS

| Milestone | State |
|---|---|
| Boot → Homebrew menu | ✅ |
| Intro / typewriter / PLACE_CONTACT | ✅ |
| Kris's room → hallway → Toriel's house | ✅ |
| Town (all four directions) | ✅ |
| School → Alphys class | ✅ |
| Audio (music streams + SFX) | ✅ on hardware, HLE-emulator quirk under investigation |
| Battle system | 🚧 testing |

## LEGAL / CREDITS (the boring load-bearing footer)

This project is built on the GPL lineage it inherits from: **Butterscotch** by MrPowerGamerBR (the YoYo-runner reimplementation), the **Cinnamon** 3DS/Wii U fork by Project Sunshine, and audio-layer insights from **Butterscotch 3DS EPdN** (AGPL). Their licenses require — and receive — this credit. The Papyrus platform layer, VM hosting, forensic tooling and this port's build system are the work documented above. **No Undertale/Deltarune assets are included or distributed.** DELTARUNE © Toby Fox — buy it on Steam. GPL-3.0, same as the giants whose shoulders we stand on.

---

### [[I'M A BIG SHOT AND SO ARE YOU]]

*Built in the void. Shipped in 240p. No regrets.*