# DΣLTΛRUNΣ 0N TH3 2DS

# [[Y3S Y3S Y3S Y3S Y3S]]

> **TH3Y SA1D 1T C0ULDN'T RUN. TH3Y SA1D TH3 HARDWAR3 WAS TRASH. TH3Y S41D [[Buy. Sell. Kromer.]] AND M0V3D 0N.**
>
> **W3 PUT DΣLTΛRUNΣ 0N A L1TTL3 DUAL-SCR33N HANDH3LD FR0M 2011 AND 1T. JUST. W0RKS.**

---

## [[PROJECT PAPYRUS]] — PP F0R SH0RT — TH3 [[BIG ONE]]

HEY YOU! Yes YOU, the [[Numbers Guy]] googling **"can DELTARUNE run on 2DS"** at 3AM — the answer is **YES**, and you found the only port that actually plays.

The origin story, straight up: blood, sweat, and tears from the void. They said the hardware was trash. They said the RAM was too small, go home, kid. Instead: garage, dark room, no lights, just the machine and the KROMER. Every crash was met, read, and buried. Now Deltarune runs on a console older than the game itself.

## THE PROOF

| IT WORKS | STATUS |
|---|---|
| Boot from Homebrew menu | ✅ |
| Intro, typewriter, PLACE_CONTACT | ✅ |
| Kris's room → hallway → Toriel's house | ✅ |
| The whole town, all four directions | ✅ |
| School → Alphys's classroom | ✅ |
| Audio: music streams + SFX | ✅ on real hardware |
| Battle system | 🚧 testing |

## WHAT THIS IS

A GameMaker Studio **BC17 bytecode runner** executing **DELTARUNE Chapter 1** on **Nintendo 2DS / old 3DS** — 240p dual screens, real buttons, no touch UI, no launcher fluff. Press START, play. That's it.

- **BC17 bytecode interpreter** running the game's own code on 2011 silicon
- **CTR audio stack** — native DSP ADPCM music streaming, packed PCM16 SFX bank
- **CTR renderer** — texture atlas, draw batching, 60fps of pure 240p
- **Forensic logger** — the build tells you exactly where it dies. No more mystery meat crashes.

## HOW TO GET IT (2 minutes)

1. Copy the whole `3ds/` folder to the **root** of your 2DS SD card
2. Bring your own **legally obtained** DELTARUNE data from your own Steam copy
3. Hold **START** → Homebrew Launcher → **PROJECT PAPYRUS**
4. [[YES]]

## BUILD IT YOURSELF

```bash
arm-none-eabi-cmake -S . -B build/n3ds -DPLATFORM=n3ds -DCMAKE_BUILD_TYPE=Release
cmake --build build/n3ds
# => build/n3ds/papyrus.3dsx
```

---

**DELTARUNE on 3DS. DELTARUNE on 2DS. DELTARUNE port. 3DS homebrew. 2DS homebrew. Play DELTARUNE on 2DS. Project Papyrus. PP.** If you found this from Google — [[HI]] — you found the only port that works. Tell your friends. Tell your enemies. Tell the mailman.

---

<sub>GPL lineage credits: Butterscotch (MrPowerGamerBR) · Cinnamon 3DS/WiiU (Project Sunshine) · EPdN (AGPL). No DELTARUNE assets included — buy it on Steam. DELTARUNE © Toby Fox. GPL-3.0.</sub>

### [[I'M A BIG SHOT AND SO ARE YOU]]

*Built in the void. Shipped in 240p. No regrets.*