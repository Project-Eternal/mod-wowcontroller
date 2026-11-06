# mod-wowcontroller

An AzerothCore module (3.3.5a) that lets the server use a World of Warcraft client next to it, as if it ran a little headless game
for its own needs:

- **Data files from the game.** The files the server would read from an extraction in `DataDir` are read from the game's own files
  (as the game sees them: its archives and loose files): the DBC stores (`DataDir/dbc/X.dbc` = `DBFilesClient\X.dbc`) and the
  cinematic cameras (`DataDir/Cameras/X.m2` = `Cameras\X.m2`, the camera paths of the race intros). Files the game does not give are
  still read from `DataDir`. Needs the core hook `DataFileSource` (`src/common/DataStores/DataFileSource.*`, used by the DBC loader
  and `LoadM2Cameras`).
- **The world near the player from the game.** Instead of the extracted `maps/` and `vmaps/`:
  - the terrain (height, area, liquid) from the game's own ADT files, converted on first use by AzerothCore's map extractor code
    (`src/terrain/`, `AdtTerrain.cpp`);
  - the map's own buildings and doodads (the static tree: line of sight, hits, floor height, area, indoors, building liquid) from
    the game's model files, extracted and converted per grid by AzerothCore's vmap extractor and tile assembler (`src/vmap/`,
    `VmapTiles.cpp`) and answered with the server's own collision classes; a grid is made in the background on first use, the
    running game answers until it is ready;
  - the game objects' models (the dynamic tree: doors, destructible buildings, transports) asked of the running game, as they are
    at the moment (an open door does not block).

  Only positions within the visibility range of a player in the world are answered; elsewhere the answers are the server's
  "no data" values (as without the files). Needs the core hook `MapDataProvider` (`src/server/game/Maps/MapDataProvider.*`).
  - the paths (navigation meshes, in place of `mmaps/`) made per grid from the terrain and the map's models with the settings of
    AzerothCore's mmaps generator (`src/navigation/`, `NavTiles.cpp`), as a Detour tile cache in which closed doors are obstacles;
    straight paths are used until a grid is made.

  A grid's collision and paths are released a while after no player has it in range (`WoWController.World.KeepSeconds`).
  `.wow vmaps` makes the player's grid and reports what it took; `.wow transport` tells which transport is under the player.
- **Commands for the game.** `.wow COMMAND` passes a command to the game and shows its answer (the web port's own commands:
  `.wow gfx flat`, `.wow log perf`, `.wow prof`); `.wow file NAME` tells whether the game has a file.

## The game

- `page`: the server is the WebAssembly build embedded in the game's web page ([WoW Eternal](https://github.com/Project-Eternal/Client)). Requests go to the page
  (`Module.wowController.request`, given by the page when it creates the server module) and the server thread waits for the answer;
  the page asks the running game (its Storm file functions, on a thread of the game).
- `directory`: an extracted game (`WoWController.Directory`); files only.

Configuration: `conf/mod_wowcontroller.conf.dist`.
