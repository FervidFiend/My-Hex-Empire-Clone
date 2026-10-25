# My-Hex-Empire-Clone

A C++ remake of **Hex Empire**, the 2007 Flash strategy game by META:SAUCE. Four nations — Redosia, Violetnam, Bluegaria and Greenland — fight over a 20×11 hex map of towns, ports and capitals. Every turn you move your armies, take towns to grow your forces, and try to capture the enemy capitals before they capture yours.

The goal is to stay close to the original: the art, fonts and sounds were pulled from the original SWF, and the layout, animations and timings follow the Flash reference.

<!-- Add a screenshot here, e.g. ![Gameplay](docs/screenshot.png) -->

## Features

- Seeded maps: type in a map number to replay the same map
- Three AI nations with adjustable difficulty
- Army manpower and morale, land and sea movement, towns and ports
- Turn-start hints, help pages and a capitulate option
- The original intro sequence and the end-of-game newspaper
- Original sound effects and music

## Controls

| Input | Action |
| --- | --- |
| Left click | Select an army, then click a nearby hex to move or attack |
| END TURN button | End your turn |
| Left / Right arrows | Flip between help pages |
| Enter | Confirm the map number |
| Space | Skip / continue |
| Esc | Close the current panel or go back |

## Building

The project is a Visual Studio 2022 solution (toolset v143, C++20) for Windows.

### Dependencies

The engine (`MyEngine`) needs these libraries:

- [GLFW](https://www.glfw.org/) and [GLEW](https://glew.sourceforge.net/) (or glad)
- [GLM](https://github.com/g-truc/glm)
- [FreeType](https://freetype.org/) and [msdfgen](https://github.com/Chlumsky/msdfgen)
- [OpenAL](https://www.openal.org/) (OpenAL Soft)
- [cereal](https://uscilab.github.io/cereal/)
- [zstd](https://github.com/facebook/zstd)
- OpenCL headers (`CL/opencl.hpp`)

The easiest way to install them is [vcpkg](https://vcpkg.io/) with Visual Studio integration:

```bash
vcpkg install glfw3 glew glm freetype msdfgen openal-soft cereal zstd opencl --triplet x64-windows
```

```bash
vcpkg integrate install
```

### Build and run

1. Open `hex empire clone (Sleepy Biden Engine v0.0).vcxproj` in Visual Studio.
2. Pick a configuration: `Debug`, `Release`, or `Release - No Console` (no console window), on `x64`.
3. Build and run. The game loads files from `resources/` by relative path, so run it with the project folder as the working directory, or copy `resources/` next to the built `.exe`.

## Project layout

| Path | Contents |
| --- | --- |
| `main.cpp` | Window setup and the main loop |
| `Game.cpp`, `Game.h` | All the game logic, AI, UI and drawing |
| `IntroTimeline.inc` | Keyframes for the intro animation |
| `NewspaperAssets.inc`, `NewspaperLayout.inc` | End-of-game newspaper assets and layout |
| `MyEngine.cpp`, `MyEngine.h` | The engine: rendering, text, audio and input |
| `resources/game/` | Images, sprites, shapes, fonts and sounds used by the game |
| `resources/other/` | Other assets taken from the SWF, kept for reference |
| `resources/parse_swf.py` | Script for looking inside the original SWF and exporting from it |

## Comparing with the original

Set `HEX_EMPIRE_CAPTURE` to a `.ppm` file path and the game draws a single frame in a hidden window, saves it to that file and quits. This is for comparing frames side by side with the Flash version.

```powershell
$env:HEX_EMPIRE_CAPTURE = "frame.ppm"; & ".\x64\Release\hex empire clone (Sleepy Biden Engine v0.0).exe"
```

`parse_swf.py` prints the SWF's buttons, fonts, text, placements, filters and sprite shapes:

```bash
python resources/parse_swf.py resources/486811_hex_empire202c.swf --section all --json
```

## Credits

- **Original game:** Hex Empire © 2007 META:SAUCE. Programming and graphics by Witold Burkiewicz; extra graphics and sound by Kamil Burkiewicz.
- **Engine:** MyEngine by FervidFiend.
- The default display font is the Potsdam typeface supplied with the game.

This is an unofficial fan project for learning purposes and is not affiliated with META:SAUCE. All the original art, sound and fonts belong to their owners.
