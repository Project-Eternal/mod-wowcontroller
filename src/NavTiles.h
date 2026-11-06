/*
 * The navigation meshes of the game's maps (what the server reads from mmaps/), made from the terrain and the map's models the module makes
 * from the game's files (AdtTerrain, VmapTiles), per grid on first use, with AzerothCore's mmaps generator's settings and steps (its
 * TerrainBuilder in navigation/) but as a Detour tile cache (navigation/DetourTileCache*): the mesh can change where game objects change,
 * a closed door is an obstacle in it, an open one is not. NavTiles.cpp.
 */
#ifndef MOD_WOWCONTROLLER_NAVTILES_H
#define MOD_WOWCONTROLLER_NAVTILES_H

#include "Define.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

class AdtTerrain;
class GameObject;
class VmapTiles;
class dtNavMesh;

class NavTiles
{
public:
    NavTiles(AdtTerrain& terrain, VmapTiles& vmaps) : _terrain(terrain), _vmaps(vmaps) { }

    // the map's navigation mesh (empty at first: its grids are added as they are made)
    std::shared_ptr<dtNavMesh> GetNavMesh(uint32 mapId);
    // grids a player witnesses (the first: the player's own): their navigation is made on the navigation thread (once each, the first
    // grid before the others waiting)
    void Want(uint32 mapId, std::vector<std::pair<uint32, uint32>> const& grids);
    // the map's update (its thread): forgets these grids' navigation (no player witnesses them any more): the tile cache's layers and the mesh's
    // tiles go (grids waiting to be made or being made are left); how many were forgotten
    uint32 Evict(uint32 mapId, std::vector<std::pair<uint32, uint32>> const& grids);
    // the map's update (its thread): the grids made meanwhile go into its mesh, the obstacles that changed are applied
    void Update(uint32 mapId, uint32 diff);

    // a game object that blocks paths while it is closed (a door) appeared, changed or went away
    static void DoorChanged(GameObject const* go, bool removed);

private:
    AdtTerrain& _terrain;
    VmapTiles& _vmaps;
};

// a line about the grids made since the last call and what they took (empty when none)
std::string DescribeNavTilesUse(bool reset);

#endif
