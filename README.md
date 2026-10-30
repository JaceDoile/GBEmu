# GB Emu

A Game Boy / Game Boy Color emulator for iPhone and iPad, for personal use.

## Features

- **Original Game Boy and Game Boy Color** games (`.gb`, `.gbc`, or inside `.zip`)
- Cartridge support: no-MBC, MBC1, MBC2, MBC3 with real-time clock, MBC5, HuC1. That covers almost the whole library, including Pokémon Red/Blue/Yellow/Gold/Silver/Crystal, Zelda, Mario, Tetris and Kirby.
- **Battery saves** are written automatically to `Files › On My iPhone › GB Emu › Saves`
- **Save import/export** compatible with mGBA, BGB, SameBoy, VBA-M, Gambatte, Delta, RetroArch (`.srm`) and flash carts (EverDrive etc.):
  - raw `.sav` / `.srm` / `.sa1` / `.dsv`
  - MBC3 clock footers (44- and 48-byte formats); the in-game clock advances by the real time elapsed since the file was saved
  - oversized flash-cart dumps are trimmed and short files padded automatically
  - every import backs up the previous save to `Saves/Backups`
- **Save states**: 9 slots with thumbnails, a resume point, and Undo Load State
- **Touch controls**: multi-touch, sticky D-pad with diagonal tuning, slide between A and B (press both in the gap), haptics, adjustable opacity
- **Controllers**: Xbox, PlayStation, Switch Pro, MFi, Backbone/Kishi. Buttons map by position (Game Boy layout) or by label. Hardware keyboards also work.
- Fast-forward (2–8×, toggle or hold), seven palettes for original Game Boy games, GBC colour correction, pixel-perfect scaling

## Installing (Windows + free Apple ID)

1. Download `GBEmu.ipa` from this repo's **Releases → latest** (every push to `main` rebuilds it on GitHub's macOS runners).
2. Install **[Sideloadly](https://sideloadly.io)** on your PC, plus iTunes and iCloud from Apple's website (not the Microsoft Store versions).
3. Plug in your iPhone, drag `GBEmu.ipa` into Sideloadly, enter your Apple ID and click **Start**.
4. On the iPhone, open **Settings › General › VPN & Device Management**, then trust your Apple ID.
   On iOS 16+ also turn on **Settings › Privacy & Security › Developer Mode** and restart.
5. A free Apple ID signs apps for **7 days**. Re-run Sideloadly before then; enabling its auto-refresh over Wi-Fi does this for you. Your games and saves stay on the phone when you refresh.

## Adding games and saves

- Tap **+** in the app, or copy files into `Files › On My iPhone › GB Emu`.
- Name a save the same as its game (`Pokemon Crystal.gbc` + `Pokemon Crystal.sav`) and it's matched automatically. You can also open a game and use **Menu › Import Save File…**.

## Project layout

```
Core/      C emulation core (CPU, PPU, APU, MBCs, RTC, saves, states)
App/       SwiftUI + UIKit iOS app
tests/     Headless test runner + save-format unit tests
project.yml  XcodeGen spec (CI generates the Xcode project)
```

Core test status: blargg `cpu_instrs`, `instr_timing`, `mem_timing`, `mem_timing-2`, `halt_bug`, `interrupt_time`, dmg-acid2 and cgb-acid2 all pass, plus 9 of 12 sound tests (the three failures are wave-RAM quirks).
