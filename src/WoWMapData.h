/*
 * The server's world as the game sees it (WoWMapData.cpp): a MapDataProvider answered by the game.
 */
#ifndef MOD_WOWCONTROLLER_WOWMAPDATA_H
#define MOD_WOWCONTROLLER_WOWMAPDATA_H

#include "MapDataProvider.h"
#include "WoWProcess.h"
#include <memory>
#include <string>
#include <vector>

// paths: the navigation meshes are made too (from the terrain and the map's models, per grid; doors as obstacles)
std::shared_ptr<MapDataProvider> CreateWoWMapData(WoWProcess* process, bool paths);

class Player;

// where the players are (only what a player witnesses is answered): its state at every update, gone at logout; true when the player has just
// become witnessed (done loading) or jumped (a teleport): its position data is to be worked out again
bool NoteWoWMapDataPlayer(Player* player, bool gone);
class GameObject;

// a game object appeared, changed or went away (the paths near doors and destructible buildings are checked with the game)
void NoteWoWMapDataGameObject(GameObject const* go, bool removed);

// before a teleport: one to another map counts as loading until the player is on the new map
void NoteWoWMapDataTeleport(Player* player, uint32 mapId);

// the same as text lines, without resetting the counts (.wow prof)
std::vector<std::string> DescribeWoWMapDataUse();

// logs how many questions were asked since the last call, how many went to the game and how long they waited, and who asked (nothing when none)
void LogWoWMapDataUse(uint32 seconds);

#endif
