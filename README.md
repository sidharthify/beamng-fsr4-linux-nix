# beamng-fsr4-linux-nix

vulkan layer that runs AMD FSR 4.1.1 in place of BeamNG.drive's "FSR 3 (custom)" upscaler in the native
Linux build of the game, packaged as a Nix flake, proton isn't supported and it doesn't touch game files. if, in case, there is a conflict (for example, an update), then it falls back to FSR3

Based on [Mob1K7105/beamng-fsr4-linux](https://github.com/Mob1K7105/beamng-fsr4-linux). The FSR 4.1.1
runtime comes from [johnpanos/FSR-4.1.1-linux](https://github.com/johnpanos/FSR-4.1.1-linux). Not affiliated
with AMD or BeamNG GmbH.

## Requirements

- BeamNG.drive 0.40, native build (`BinLinux/BeamNG.drive.x64`)
- Anti-aliasing set to **FSR 3 (custom)**, any render scale
- An AMD RDNA 2+ GPU on RADV with `shaderInt8` and `shaderIntegerDotProduct`. With
  `VK_VALVE_shader_mixed_float_dot_product` the output matches AMD's DLL exactly. Without it, a portable
  prepass is used.
- Output up to 3840×2160

Tested on an RX 6700 XT (upstream) and an RX 9060 XT, both on Mesa RADV.

## NixOS / home-manager

```nix
# flake.nix
inputs.bng-fsr4.url = "github:sidharthify/beamng-fsr4-linux-nix";

# home-manager
imports = [ inputs.bng-fsr4.homeManagerModules.default ];
programs.bng-fsr4.enable = true;
```

This installs the layer manifest to `~/.local/share/vulkan/implicit_layer.d` and the menu-label mod to
BeamNG's `mods` folder (`menuMod = false` skips the mod, `userFolder` moves it). The layer does nothing
until the game is started with `BNG_FSR4=1`:

- Steam launch options: `BNG_FSR4=1 %command%`
- Or in a launcher script that wraps the game in `steam-run`: `export BNG_FSR4=1`

Without home-manager, point the loader at the package directly:

```sh
nix build github:sidharthify/beamng-fsr4-linux-nix
BNG_FSR4=1 VK_ADD_IMPLICIT_LAYER_PATH=$PWD/result/share/vulkan/implicit_layer.d steam-run ./BinLinux/BeamNG.drive.x64
```

## Other distros

```sh
make && make install PREFIX=$HOME/.local
cp ~/.local/share/bng-fsr4/bng_fsr4_linux.zip ~/.local/share/BeamNG/BeamNG.drive/current/mods/   # optional
```

You need g++ and Vulkan headers 1.4.357 or newer. `PREFIX` has to be the final location, because the
manifest refers to the library by its absolute path.

## Checking it works

`~/.cache/bng-fsr4/events.log` logs `FSR4: backend constructed`, then every 600 frames:

```
FSR4: frames replaced 4801, fail-open 8, skipped chains 4800, soft 0, hard 0, jitter checked 4800 mismatched 0
```

`fail-open` counts frames that FSR 3 rendered (start-up, resets). `hard` should stay 0. With the mod
enabled, the anti-aliasing menu shows "FSR 4.1.1 (Linux)" while the layer is running.

## Options

| Environment variable       | Effect                                                        |
|----------------------------|---------------------------------------------------------------|
| `BNG_FSR4=1`               | load the layer (required)                                     |
| `BNG_FSR4_DISABLE=1`       | don't load it even when `BNG_FSR4` is set                     |
| `BNG_FSR4_SHARPNESS=0.5`   | fixed sharpness 0–1 instead of following the game's slider    |
| `BNG_FSR4_NO_SHARPEN=1`    | turn off FSR 4's RCAS                                         |
| `BNG_FSR4_KEEP_FSR3=1`     | keep dispatching FSR 3's passes too (slower, for comparison)  |
| `BNG_FSR4_ASSETS=dir`      | load the FSR 4.1.1 assets from `dir`                          |
| `BNG_FSR4_USERFOLDER=dir`  | BeamNG user folder, for the menu-label status file            |

While the game runs, creating `~/.cache/bng-fsr4/fsr4.disable` switches back to FSR 3 (after a 32-frame
handover), and deleting it switches FSR 4 back on. `fsr3skip.disable` keeps FSR 3's passes running
underneath FSR 4.

## Limitations

- FSR 4 costs more GPU time than FSR 3. Upstream measured about 1.4 ms per frame more at 1440p on an
  RX 6700 XT. RDNA 2 has no WMMA/FP8, so the INT8 model runs on packed dot products. Pair it with a lower
  render scale if you need the frames back.
- After a reset that keeps the same size (e.g. toggling the upscaler), FSR 3 renders for 17–32 frames while
  the jitter sequence is relearned.
- Changing the output resolution while the game is running, HDR, VR and multi-monitor are untested.
- The shader fingerprints are for BeamNG 0.40. After a game update the layer falls back to FSR 3 until
  they are updated.

How the interception works: [docs/how-it-works.md](docs/how-it-works.md). Licensing:
[docs/licensing.md](docs/licensing.md). The layer is GPL-2.0-or-later. The FSR 4.1.1 shader and model
assets are under AMD's MIT exception for the upscaler DLL.
