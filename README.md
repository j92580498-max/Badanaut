<!--
Copyright (C) 2026 Badanaut Emulator Project
SPDX-License-Identifier: Apache-2.0 OR MIT
-->

# Badanaut

<p align="center">
  <img src="./frontends/pocket-desktop/assets/badanaut-logo.png" width="25%" alt="Badanaut logo" />
</p>

<p align="center">
  A high-level Samsung bada OS emulator for Linux, Windows and Android.
</p>

<p align="center">
  <a href="https://discord.gg/YOUR_INVITE">
    <img src="https://img.shields.io/badge/Discord-Join%20our%20Community-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Join the Badanaut Discord community" />
  </a>
</p>

<p align="center">
  <strong>Join our Discord for development updates, compatibility discussions, support and community chat.</strong>
</p>

---

> [!NOTE]
> Badanaut targets native 32-bit ARM Samsung bada applications packaged as `.ZIP` archives. It is experimental software: compatibility, performance and input behavior vary by game and host platform.

> [!WARNING]
> Badanaut is developed for research and educational purposes. It does not include Samsung system files, firmware, the bada runtime or game data. Use only legally obtained game copies and archives.

## Info

Badanaut is an early-stage high-level emulator for Samsung bada games. Instead of emulating a complete bada device, it loads the original game executable, runs its ARM code through a CPU backend, and provides host-side implementations of the bada APIs the game expects.

The engine is written in C++17 and uses [Unicorn](https://github.com/unicorn-engine/unicorn) as the CPU backend. The desktop host is built with Python and Qt, and the Android host with Kotlin and SDL.



## Games Tested

| Sky Force | Fruit Ninja |
| :---: | :---: |
| ![Sky Force](./proof/games/sky-force.jpg) | ![Fruit Ninja](./proof/games/fruit-ninja.jpg) |

| N.O.V.A. | Exploration |
| :---: | :---: |
| ![N.O.V.A.](./proof/games/nova.jpg) | ![Exploration](./proof/games/exploration.jpg) |

