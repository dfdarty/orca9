# Orca-9: Pod Commander

A 3D space shooter for the [FREE-WILi 2](https://freewili.com). You fly an orca-shaped starship, steered by tilting the device, through waves of asteroids and alien Stingers, with a manta-ray Guardian Angel drone guarding your back.

It is a WiliBSP DISPLAY-CPU app (a UF2 installed under `/apps/`), developed on the unofficial [freewili2-emu](https://github.com/dfdarty/freewili2-emu) emulator.

## Status: first playable (v001)

- Title, hangar ship select, game over.
- Four selectable hulls, each with its own stats, primary weapon, special, weakness and roll:

  | Hull | Weapon | Special (CANCEL) | Weakness | Roll |
  |---|---|---|---|---|
  | APEX | twin jaw railguns (overheat) | Overdrive: all guns, 5 s | master of none | 0.5 s, 2 s cooldown |
  | BLACKFISH | wingtip lasers | Ghost Burn: 2× speed, fire misses you | 50 HP | 0.35 s, 1.2 s, 2 charges |
  | MATRIARCH | homing missiles + auto flak | Pod Call: full manta shield ring | slow, big | 0.9 s, 4 s |
  | TIDEBREAKER | short-range 3-way jaw cannons | Breach: jaw ram charge | short reach, slow FTL | rolls ram enemies |

- Barrel roll with invulnerable middle frames; a *perfect roll* (dodging a hit mid-roll) refills charges and boosts damage for 2 s.
- Guardian Angel manta shoots down the nearest incoming bolt.
- Waves of 35 s, FTL jump (blue key) once the FTL bar fills, radar, score.

Not yet: bosses and their unique weapons, power-ups, the wormhole and free-flight ("all-range") levels, mini black holes, sound, saved high scores.

## Controls

| Input | In flight | In menus |
|---|---|---|
| Tilt | steer | |
| Touch and drag | steer (a virtual stick from where your finger lands) | tap a key label |
| OK (hold) | fire | select |
| D-pad ◄ ► | barrel roll | previous / next hull |
| D-pad ▲ ▼ | boost / brake | |
| CANCEL | special | back |
| PAGE | re-centre tilt | |
| Blue key | FTL jump when ready | next hull |
| Yellow / green / grey | | previous / launch / back |
| HOME held 5 s | exit | exit |

Tilt is centred on whatever angle you hold the device at launch ("HOLD STEADY").

In the emulator: arrow keys are the D-pad, O is OK, C is CANCEL, 1–5 the colour keys, and a mouse drag on the screen steers.

## Build and run

With [freewili2-emu](https://github.com/dfdarty/freewili2-emu) checked out next to this folder:

```sh
tools/fw2emu run ../orca9          # play it in a window
tools/fw2emu test ../orca9         # run test.txt headless
tools/fw2emu hwcheck ../orca9      # build the UF2 with the Pico SDK + Arm GCC and check it fits
```

The UF2 lands in `build-hw/apps/orca9/orca9.uf2`; install it with WiliBSP's `fw install-app`.

## How it draws

A small software renderer (`render.c`): flat-shaded triangles sorted back to front, drawn in eight 480×40 bands. While one band goes to the LCD by DMA, the next is drawn. Player hulls are 280–400 triangles, the manta 38.

The meshes in `meshes.c` are generated from `tools/ships.js`, the same geometry as the concept art:

```sh
node tools/genmesh.js
```

## Performance (emulator estimate, RP2350 at 250 MHz)

| Build | Frame rate | Limit |
|---|---|---|
| default (WiliBSP AgentIO on) | ~14 fps | AgentIO copies every flushed band into its PSRAM shadow one pixel at a time: ~4 ms a band |
| AgentIO off (`-DFW2_AGENTIO=OFF`) | ~20 fps | the LCD's SPI wire time: 39 ms per full 480×320 frame at 62.5 MHz |

The game logic and 3D setup take about 1–3 ms a frame. To be confirmed on a real board.

## Licence

MIT.
