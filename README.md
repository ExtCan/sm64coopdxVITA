![sm64coopdx Logo](textures/segment2/custom_coopdx_logo.rgba32.png)

# sm64coopdx for the PlayStation Vita

This fork ports [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx) to the PS Vita.

**It is experimental.** It has been run on a real Vita: it boots, renders, plays sound and loads
mods. 30 FPS holds in large levels, 60 FPS only in smaller scenes. Direct connection netplay is
implemented but untested, and CoopNet (public lobbies) is not available yet.

* **Install, controls, known problems and build instructions: [README_vita.md](README_vita.md)**
* You need a hacked Vita with `libshacccg.suprx` installed and your own US Super Mario 64 ROM.
  No ROM or game assets are included in this repository.
* Build with VitaSDK: `make TARGET_VITA=1` produces `build/us_vita/sm64coopdx.vpk`.

Everything below is the README of the upstream project.

---

sm64coopdx is an online multiplayer project for the Super Mario 64 PC port that synchronizes all entities and every level for multiple players. The project was started by the Coop Deluxe Team. The purpose is to actively maintain and improve, but also continue sm64ex-coop, created by djoslin0. More features, customization, and power to the Lua API allow modders and players to enjoy Super Mario 64 more than ever!

Feel free to report bugs or contribute to the project.

## Initial Goal (Accomplished)
Create a mod for the PC port where multiple people can play together online.

Unlike previous multiplayer projects, this one synchronizes enemies and events. This allows players to interact with the same world at the same time.

Interestingly enough though, the goal of the project has slowly evolved over time from simply just making a Super Mario 64 multiplayer mod to constantly maintaining and improving the project (notably the Lua API.)

## Documentation

sm64coopdx is moddable via Lua, similar to Roblox and Garry's Mod's Lua APIs. To get started, click [here](docs/lua/lua.md) to see the Lua documentation. If you want to contribute to the repo, you can view the C documentation [here](docs/c/c.md).

## Wiki
The wiki is made using GitHub's wiki feature, you can go to the wiki tab or click [here](https://github.com/coop-deluxe/sm64coopdx/wiki).

## Community
We have an official Discord server open to the public [here](https://discord.gg/TJVKHS4).
