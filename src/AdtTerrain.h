/*
 * The terrain of the game's maps for the server, made from the game's own ADT files on first use (AdtTerrain.cpp): what the server reads
 * from an extracted maps/MMMXXYY.map, without the extraction.
 */
#ifndef MOD_WOWCONTROLLER_ADTTERRAIN_H
#define MOD_WOWCONTROLLER_ADTTERRAIN_H

#include "Optional.h"
#include "GridTerrainData.h"   // (after Optional.h: it uses Optional)
#include "WoWProcess.h"
#include <memory>

class AdtTerrain
{
public:
    explicit AdtTerrain(WoWProcess* process) : _process(process) { }

    // the terrain of the grid that holds x, y on the map: loaded (read from the game, converted) the first time; nullptr when the map has no
    // terrain there (no ADT: ocean, a map of a single building) or the game does not give it
    std::shared_ptr<GridTerrainData> Get(uint32 mapId, float x, float y);

private:
    WoWProcess* _process;
};

// the directory the terrain tiles made are kept in, as the server names them (MMMXXYY.map: map, grid x, grid y)
std::string AdtTerrainMapsDirectory();

// the directory of a map's files in the game (Map.dbc field 1: World\\Maps\\<directory>\\...): empty when unknown
std::string GameMapDirectory(WoWProcess* process, uint32 mapId);

// the game's Map.dbc as the DBC loader has just read it from the game: the folder names are kept for GameMapDirectory
void NoteGameMapDbc(std::vector<uint8> const& dbc);

// logs the tiles made since the last call (nothing when none)
void LogAdtTerrainUse();

#endif
