/*
 * The collision of the map's own buildings and doodads (what the server reads from vmaps/: its static tree), made from the game's own files
 * on first use, per grid (VmapTiles.cpp): AzerothCore's vmap extractor (vmap/) reads the grid's ADT placements and the models they name, its
 * tile assembler converts them, and the server's own collision classes (VMAP::ModelInstance, WorldModel) answer with them.
 */
#ifndef MOD_WOWCONTROLLER_VMAPTILES_H
#define MOD_WOWCONTROLLER_VMAPTILES_H

#include "Define.h"
#include "WoWProcess.h"
#include <memory>
#include <string>
#include <vector>

namespace VMAP
{
    class ModelInstance;
}

// the models placed in a grid (a model reaching into several grids is in each of them, as the same instance)
struct VmapTile
{
    std::vector<std::shared_ptr<VMAP::ModelInstance const>> instances;
};

class VmapTiles
{
public:
    explicit VmapTiles(WoWProcess* process) : _process(process) { }

    // the static collision of grid (gx, gy) of the map, made now if it is not yet (a map of a single building: its building, whatever the
    // grid); nullptr when the game does not give it
    std::shared_ptr<VmapTile const> Get(uint32 mapId, uint32 gx, uint32 gy);
    // the same if it is made (ready: true; nullptr when the game does not give it); else ready false and it is made on the maker thread
    std::shared_ptr<VmapTile const> TryGet(uint32 mapId, uint32 gx, uint32 gy, bool& ready);

    // forgets the grid (on the maker thread): a player no longer witnesses it; the models only it used are released
    void Evict(uint32 mapId, uint32 gx, uint32 gy);

private:
    WoWProcess* _process;
};

// a line about the tiles made since the last call and what they took (empty when none)
std::string DescribeVmapTilesUse(bool reset);

#endif
