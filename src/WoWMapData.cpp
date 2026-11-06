/*
 * The server's world as the game sees it (MapDataProvider, the core's hook), instead of extracted maps/ and vmaps/: the terrain (height, area,
 * liquid) from the game's own ADT files, converted on first use (AdtTerrain.cpp); line of sight and hits with the models asked of the running
 * game (WoWProcess::WorldQueries: they run while its game thread sleeps).
 * Only what the player can witness is answered: a question is answered when a player is in the world (not loading, not in a far teleport), on
 * the question's map, and the position (both ends of a line) is within the map's visibility range of the player. Everything else gets the
 * server's "no data" values at once (what it sees without the files), as does what the game cannot tell: the server simulates in detail what
 * is in effect for the player, and the world beyond as if it had no map data.
 * The building (WMO) a position is in (GetAreaAndLiquidData: its group's ids and flags, its floor and liquid) is asked of the game too; the
 * server works out the area (WMOAreaTable), indoors and the liquid from it as it would from vmaps.
 * The server keeps the map's models (vmaps: the static tree) apart from the game objects' (doors, transports, destructible buildings: the
 * dynamic tree), and asks either or both. The game's flags: the map's doodads 0x1, game objects' models 0x100000 (as they are now), buildings
 * 0x10 for both sets (a game object's building is a placement like the map's): the questions name their set (SetMap / SetGameObjects) and
 * the game passes through the hits on buildings of the other set. The transport under a position is a trace of the game objects that hits
 * within the transport's bounds.
 * Statistics once a minute (LogWoWMapDataUse): questions per kind, how many went to the game and how long they waited, and the functions of
 * the server that ask (a call stack of one question in SampleEvery).
 */
#include "WoWMapData.h"
#include "AdtTerrain.h"
#include "Config.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridDefines.h"
#include "IVMapMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapTree.h"
#include "NavTiles.h"
#include "ModelIgnoreFlags.h"
#include "ModelInstance.h"
#include "Player.h"
#include "VMapMgr2.h"
#include "VmapTiles.h"
#include "WorldModel.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace
{
    // ---- where the players are (WoWControllerPlayers: updated on the players' own map threads, read by any thread) ----

    struct PlayerView
    {
        uint32 map = 0;
        float x = 0.0f, y = 0.0f;
        float range = 0.0f;     // the map's visibility range
        bool loading = true;    // logging in, in a far teleport, not in the world
    };

    std::mutex g_playersLock;
    std::unordered_map<uint64, PlayerView> g_players;

    // ---- statistics ----

    // (the model questions come in two sets, the map's models and the game objects': the second kind of each pair)
    enum Kind { KindTerrainHeight, KindTerrainMinHeight, KindTerrainArea, KindLiquidLevel, KindLiquidData, KindLineOfSight,
        KindLineOfSightObjects, KindObjectHit, KindObjectHitObjects, KindModelHeight, KindModelHeightObjects, KindBuilding, KindBuildingObjects,
        KindGameObject, KindPathCheck, KindCount };
    char const* const KindNames[KindCount] = { "terrain height", "terrain minimum height", "terrain area", "liquid level", "liquid data",
        "line of sight", "line of sight (game objects)", "object hit", "object hit (game objects)", "model height", "model height (game objects)",
        "building area and liquid", "building area and liquid (game objects)", "game object (transport) hit", "path checked near game objects" };
    bool const KindAsksGame[KindCount] = { false, false, false, false, false, true, true, true, true, true, true, true, true, true, true };

    // ---- the game objects a navigation mesh may not know as they are (doors, destructible buildings): paths near them are checked
    // with the game (CheckPath); kept from the game objects' hooks (NoteWoWMapDataGameObject, the maps' threads) ----

    struct ObjectView
    {
        float x, y, z, radius;
    };

    std::mutex g_objectsLock;
    std::unordered_map<uint32, std::unordered_map<uint64, ObjectView>> g_objects;   // by map, by guid

    using Models = MapDataProvider::Models;

    // the game's trace flags for one of the server's two sets of models (doodads: the doodad models of the set too)
    uint32 TraceFlags(Models models, bool doodads)
    {
        if (models == Models::Map)
            return WoWWorldQuery::SetMap | WoWWorldQuery::TraceBuildings | (doodads ? WoWWorldQuery::TraceDoodads : 0);
        return WoWWorldQuery::SetGameObjects | WoWWorldQuery::TraceBuildings | (doodads ? WoWWorldQuery::TraceGameObjects : 0);
    }

    struct KindCounts
    {
        std::atomic<uint64> asked{0}, sent{0}, unknown{0}, waitUs{0}, maxWaitUs{0};
        std::atomic<uint64> local{0}, agree{0}, disagree{0};   // answered from the grids made from the game's files; compared with the game
    };
    KindCounts g_counts[KindCount];

    // 1 in this many questions answered from the made grids is asked of the game too and the answers compared (0: none; .conf
    // WoWController.World.Compare)
    std::atomic<uint32> g_compareEvery{0};
    std::atomic<uint64> g_compareTick{0};
    std::atomic<uint32> g_mismatchesLogged{0};
    // how long a grid stays after the last player had it in range (WoWController.World.KeepSeconds)
    std::atomic<int64> g_keepMs{60000};

    std::atomic<int64> g_windowStartMs{0};   // when the counts were last logged (steady clock, ms)

    int64 NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    constexpr uint64 SampleEvery = 1000;
    std::atomic<uint64> g_sampleTick{0};
    std::mutex g_callersLock;
    std::unordered_map<std::string, uint64> g_callers;

    // the server's functions that asked (a few frames above this module), from a call stack of the calling thread
    void SampleCaller(Kind kind, bool sent)
    {
#ifdef __EMSCRIPTEN__
        static thread_local char stack[16384];
        EM_ASM({ Error.stackTraceLimit = 64; });   // (V8 keeps 10 frames by default: the JS frames of the call stack function use most of them)
        if (emscripten_get_callstack(EM_LOG_C_STACK, stack, sizeof(stack)) <= 0)
            return;
        static std::atomic<bool> shown{false};
        if (!shown.exchange(true))
            LOG_INFO("module", "A sampled call stack (the first): {}", std::string(stack).substr(0, 1500));
        std::istringstream lines(stack);
        std::vector<std::string> frames;
        bool past = false;
        for (std::string line; std::getline(lines, line) && frames.size() < 5; )
        {
            // "    at NAME (URL)" (V8) or "NAME@URL" (Firefox); the parameter lists are left out
            std::size_t start = line.find_first_not_of(' ');
            if (start == std::string::npos)
                continue;
            std::string name = line.substr(start);
            if (name.compare(0, 3, "at ") == 0)
                name = name.substr(3, name.find(" (") == std::string::npos ? std::string::npos : name.find(" (") - 3);
            else if (std::size_t at = name.find('@'); at != std::string::npos)
                name = name.substr(0, at);
            for (std::size_t anonymous; (anonymous = name.find("(anonymous namespace)::")) != std::string::npos; )
                name.erase(anonymous, 22);
            if (std::size_t paren = name.find('('); paren != std::string::npos)
                name = name.substr(0, paren);
            bool ours = name.find("WoWMapData") != std::string::npos || name.find("SampleCaller") != std::string::npos ||
                name.find("callstack") != std::string::npos || name.find("Callstack") != std::string::npos ||
                name.find("jsStackTrace") != std::string::npos || name.find("stackTrace") != std::string::npos;
            if (ours)
            {
                past = true;
                continue;
            }
            if (past && !name.empty())
                frames.push_back(name);
        }
        std::string key = std::string(KindNames[kind]) + (sent ? " [sent]" : " [no data]") + ":";
        for (std::string const& frame : frames)
            key += " < " + frame;
        std::lock_guard<std::mutex> guard(g_callersLock);
        ++g_callers[key];
#else
        (void) kind;
        (void) sent;
#endif
    }

    class WoWMapData : public MapDataProvider
    {
    public:
        WoWMapData(WoWProcess* process, bool paths) : _process(process), _terrain(process), _vmaps(process), _nav(_terrain, _vmaps), _paths(paths) { }

        bool HasTerrain(uint32 /*mapId*/, uint32 /*gx*/, uint32 /*gy*/) override
        {
            return true;   // (whether there is an answer is decided per question)
        }

        float GetTerrainHeight(uint32 mapId, float x, float y) override
        {
            std::shared_ptr<GridTerrainData> terrain = Terrain(KindTerrainHeight, mapId, x, y);
            return terrain ? terrain->getHeight(x, y) : INVALID_HEIGHT;
        }

        float GetTerrainMinHeight(uint32 mapId, float x, float y) override
        {
            std::shared_ptr<GridTerrainData> terrain = Terrain(KindTerrainMinHeight, mapId, x, y);
            return terrain ? terrain->getMinHeight(x, y) : MIN_HEIGHT;
        }

        uint16 GetTerrainArea(uint32 mapId, float x, float y) override
        {
            std::shared_ptr<GridTerrainData> terrain = Terrain(KindTerrainArea, mapId, x, y);
            return terrain ? terrain->getArea(x, y) : 0;
        }

        float GetTerrainLiquidLevel(uint32 mapId, float x, float y) override
        {
            std::shared_ptr<GridTerrainData> terrain = Terrain(KindLiquidLevel, mapId, x, y);
            return terrain ? terrain->getLiquidLevel(x, y) : INVALID_HEIGHT;
        }

        LiquidData GetTerrainLiquidData(uint32 mapId, float x, float y, float z, float collisionHeight, Optional<uint8> reqLiquidType) override
        {
            std::shared_ptr<GridTerrainData> terrain = Terrain(KindLiquidData, mapId, x, y);
            return terrain ? terrain->GetLiquidData(x, y, z, collisionHeight, reqLiquidType) : LiquidData();
        }

        bool IsInLineOfSight(uint32 mapId, Models models, float x1, float y1, float z1, float x2, float y2, float z2,
            VMAP::ModelIgnoreFlags ignoreFlags) override
        {
            if (x1 == x2 && y1 == y2 && z1 == z2)
                return true;   // (as the vmaps: a line of no length is clear; the game's trace may say it hits)
            std::vector<VMAP::ModelInstance const*> local;
            std::vector<std::shared_ptr<VmapTile const>> holders;   // (the grids stay while the answer is worked out)
            if (models == Models::Map && Local(KindLineOfSight, mapId, x1, y1, x2, y2, local, holders))
            {
                bool clear = LocalLineOfSight(local, x1, y1, z1, x2, y2, z2, ignoreFlags);
                if (Compare())
                {
                    bool doodads = (ignoreFlags & VMAP::ModelIgnoreFlags::M2) == VMAP::ModelIgnoreFlags::Nothing;
                    WoWWorldQuery q = Trace(mapId, TraceFlags(models, doodads), x1, y1, z1, x2, y2, z2);
                    if (_process->WorldQueries(&q, 1) && q.known)
                        Compared(KindLineOfSight, clear == !q.a[0], mapId, x1, y1, z1, x2, y2, z2, clear ? "clear" : "blocked",
                            q.a[0] ? "blocked" : "clear");
                }
                return clear;
            }
            bool doodads = (ignoreFlags & VMAP::ModelIgnoreFlags::M2) == VMAP::ModelIgnoreFlags::Nothing;
            WoWWorldQuery q = Trace(mapId, TraceFlags(models, doodads), x1, y1, z1, x2, y2, z2);
            Ask(models == Models::Map ? KindLineOfSight : KindLineOfSightObjects, &q, 1);
            return !(q.known && q.a[0]);
        }

        bool GetObjectHitPos(uint32 mapId, Models models, float x1, float y1, float z1, float x2, float y2, float z2, float& rx, float& ry,
            float& rz, float modifyDist) override
        {
            // as StaticMapTree::GetObjectHitPos: the hit moved by modifyDist along the line (back towards the start when negative)
            rx = x2;
            ry = y2;
            rz = z2;
            float dx = x2 - x1, dy = y2 - y1, dz = z2 - z1, length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (length < 1e-10f)
                return false;
            std::vector<VMAP::ModelInstance const*> local;
            std::vector<std::shared_ptr<VmapTile const>> holders;   // (the grids stay while the answer is worked out)
            if (models == Models::Map && Local(KindObjectHit, mapId, x1, y1, x2, y2, local, holders))
                return LocalObjectHitPos(local, x1, y1, z1, x2, y2, z2, rx, ry, rz, modifyDist);
            WoWWorldQuery q = Trace(mapId, TraceFlags(models, true), x1, y1, z1, x2, y2, z2);
            Ask(models == Models::Map ? KindObjectHit : KindObjectHitObjects, &q, 1);
            if (!q.known || !q.a[0])
                return false;
            dx /= length;
            dy /= length;
            dz /= length;
            float hx = q.f[0], hy = q.f[1], hz = q.f[2];
            float hit = std::sqrt((hx - x1) * (hx - x1) + (hy - y1) * (hy - y1) + (hz - z1) * (hz - z1));
            if (modifyDist < 0 && hit <= -modifyDist)
            {
                rx = x1;
                ry = y1;
                rz = z1;
            }
            else
            {
                rx = hx + dx * modifyDist;
                ry = hy + dy * modifyDist;
                rz = hz + dz * modifyDist;
            }
            return true;
        }

        float GetModelHeight(uint32 mapId, Models models, float x, float y, float z, float maxSearchDist) override
        {
            std::vector<VMAP::ModelInstance const*> local;
            std::vector<std::shared_ptr<VmapTile const>> holders;   // (the grids stay while the answer is worked out)
            if (models == Models::Map && Local(KindModelHeight, mapId, x, y, x, y, local, holders))
            {
                float height = LocalHeight(local, x, y, z, maxSearchDist);
                if (Compare())
                {
                    WoWWorldQuery q = Trace(mapId, TraceFlags(models, true), x, y, z, x, y, z - maxSearchDist);
                    if (_process->WorldQueries(&q, 1) && q.known)
                    {
                        float game = q.a[0] ? q.f[2] : VMAP_INVALID_HEIGHT_VALUE;
                        Compared(KindModelHeight, std::fabs(game - height) < 0.5f, mapId, x, y, z, x, y, z - maxSearchDist,
                            std::to_string(height), std::to_string(game));
                    }
                }
                return height;
            }
            WoWWorldQuery q = Trace(mapId, TraceFlags(models, true), x, y, z, x, y, z - maxSearchDist);
            Ask(models == Models::Map ? KindModelHeight : KindModelHeightObjects, &q, 1);
            return q.known && q.a[0] ? q.f[2] : VMAP_INVALID_HEIGHT_VALUE;
        }

        bool GetAreaAndLiquidData(uint32 mapId, Models models, float x, float y, float z, Optional<uint8> reqLiquidType,
            VMAP::AreaAndLiquidData& data) override
        {
            std::vector<VMAP::ModelInstance const*> local;
            std::vector<std::shared_ptr<VmapTile const>> holders;   // (the grids stay while the answer is worked out)
            if (models == Models::Map && Local(KindBuilding, mapId, x, y, x, y, local, holders))
            {
                bool found = LocalAreaAndLiquidData(local, x, y, z, reqLiquidType, data);
                if (Compare())
                {
                    WoWWorldQuery q = Query(WoWWorldQuery::Building, mapId, TraceFlags(models, false), x, y, z);
                    if (_process->WorldQueries(&q, 1) && q.known)
                    {
                        uint32 groupId;
                        memcpy(&groupId, &q.f[1], 4);
                        bool same = found == bool(q.a[0]) && (!found || (data.areaInfo && data.areaInfo->rootId == int32(q.a[1]) &&
                            data.areaInfo->groupId == int32(groupId) && std::fabs(data.floorZ - q.f[0]) < 0.5f));
                        Compared(KindBuilding, same, mapId, x, y, z, x, y, z,
                            found && data.areaInfo ? fmt::format("root {} group {} floor {}", data.areaInfo->rootId, data.areaInfo->groupId, data.floorZ) : "none",
                            q.a[0] ? fmt::format("root {} group {} floor {}", q.a[1], groupId, q.f[0]) : "none");
                    }
                }
                return found;
            }
            // the building group below (as the game's own area lookup) and its liquid; the server takes the area (WMOAreaTable), whether it
            // is indoors and the liquid from them as from vmaps
            uint32 traceFlags = TraceFlags(models, false);
            WoWWorldQuery q[2] = { Query(WoWWorldQuery::Building, mapId, traceFlags, x, y, z),
                Query(WoWWorldQuery::BuildingLiquid, mapId, traceFlags, x, y, z) };
            Ask(models == Models::Map ? KindBuilding : KindBuildingObjects, q, 2);
            if (!q[0].known || !q[0].a[0])
                return false;
            uint32 groupId, flags;
            memcpy(&groupId, &q[0].f[1], 4);
            memcpy(&flags, &q[0].f[2], 4);
            data.floorZ = q[0].f[0];
            data.areaInfo.emplace(int32(groupId), int32(q[0].a[2]), int32(q[0].a[1]), flags, 0);
            if (q[1].known && q[1].a[0] && (!reqLiquidType || (GetLiquidFlags(q[1].a[1]) & *reqLiquidType)))
                data.liquidInfo.emplace(q[1].a[1], q[1].f[0]);
            return true;
        }

        bool IntersectGameObject(GameObject const& go, float x, float y, float z, float dx, float dy, float dz, float& maxDist) override
        {
            // the game's trace with the game objects only (it does not say which one it hit): a hit counts when it is within the object's
            // bounds (GameObjectDisplayInfo, as the server places and turns its model: position, facing, scale), or anywhere when the
            // display has no bounds (some transports' have none)
            WoWWorldQuery q = Trace(go.GetMapId(), TraceFlags(Models::GameObjects, true), x, y, z, x + dx * maxDist, y + dy * maxDist, z + dz * maxDist);
            Ask(KindGameObject, &q, 1);
            if (!q.known || !q.a[0])
                return false;
            float hx = q.f[0], hy = q.f[1], hz = q.f[2];
            if (GameObjectDisplayInfoEntry const* info = sGameObjectDisplayInfoStore.LookupEntry(go.GetDisplayId()))
                if (info->maxX > info->minX && info->maxY > info->minY && info->maxZ > info->minZ)
                {
                    float const slack = 1.0f;   // (yards: the game's and the server's positions of a moving transport differ a little)
                    float scale = go.GetObjectScale() > 0.0f ? go.GetObjectScale() : 1.0f;
                    float o = go.GetOrientation(), c = std::cos(o), s = std::sin(o);
                    float ox = hx - go.GetPositionX(), oy = hy - go.GetPositionY(), oz = hz - go.GetPositionZ();
                    float lx = (c * ox + s * oy) / scale, ly = (c * oy - s * ox) / scale, lz = oz / scale;
                    float margin = slack / scale;
                    if (lx < info->minX - margin || lx > info->maxX + margin || ly < info->minY - margin || ly > info->maxY + margin ||
                        lz < info->minZ - margin || lz > info->maxZ + margin)
                        return false;
                }
            float dist = std::sqrt((hx - x) * (hx - x) + (hy - y) * (hy - y) + (hz - z) * (hz - z));
            if (dist > maxDist + 0.01f)
                return false;   // (not within the ray's length)
            maxDist = dist;
            return true;
        }

        // ---- paths: navigation meshes made from the game's files (NavTiles) ----

        // a path's segments that pass near a game object the mesh may not know as it is now (NoteWoWMapDataGameObject) are traced in the
        // game (its game objects, at a walker's height): the first blocked one, and where to stop before the block
        bool CheckPath(uint32 mapId, std::vector<G3D::Vector3> const& points, std::size_t& segment, G3D::Vector3& stop) override
        {
            if (!_paths || points.size() < 2)
                return false;
            std::vector<ObjectView> objects;
            {
                std::lock_guard<std::mutex> guard(g_objectsLock);
                auto found = g_objects.find(mapId);
                if (found == g_objects.end())
                    return false;
                for (auto const& [guid, view] : found->second)
                    objects.push_back(view);
            }
            float const height = 1.5f, back = 0.5f;   // (yards: the trace's height above the path, how far before a block to stop)
            for (std::size_t i = 0; i + 1 < points.size(); ++i)
            {
                G3D::Vector3 const& a = points[i];
                G3D::Vector3 const& b = points[i + 1];
                bool near = false;
                for (ObjectView const& o : objects)
                {
                    // (the distance from the object to the segment, flat)
                    float dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
                    float t = length2 > 0.0f ? std::clamp(((o.x - a.x) * dx + (o.y - a.y) * dy) / length2, 0.0f, 1.0f) : 0.0f;
                    float px = a.x + dx * t - o.x, py = a.y + dy * t - o.y;
                    if (px * px + py * py <= (o.radius + 1.0f) * (o.radius + 1.0f) && std::fabs(a.z + (b.z - a.z) * t - o.z) <= o.radius + 5.0f)
                    {
                        near = true;
                        break;
                    }
                }
                if (!near)
                    continue;
                WoWWorldQuery q = Trace(mapId, TraceFlags(Models::GameObjects, true), a.x, a.y, a.z + height, b.x, b.y, b.z + height);
                Ask(KindPathCheck, &q, 1);
                if (!q.known || !q.a[0])
                    continue;
                G3D::Vector3 dir = b - a;
                float length = dir.length();
                if (length < 1e-4f)
                    continue;
                float hit = std::sqrt((q.f[0] - a.x) * (q.f[0] - a.x) + (q.f[1] - a.y) * (q.f[1] - a.y) + (q.f[2] - a.z - height) * (q.f[2] - a.z - height));
                segment = i;
                stop = a + dir * (std::max(hit - back, 0.0f) / length);
                return true;
            }
            return false;
        }

        std::shared_ptr<dtNavMesh> GetNavMesh(uint32 mapId) override
        {
            return _paths ? _nav.GetNavMesh(mapId) : nullptr;
        }

        // (the grids are made where players are, not where the server loads them: UpdateNavMesh)
        void NavMeshGridLoaded(uint32 /*mapId*/, uint32 /*gx*/, uint32 /*gy*/) override { }

        void UpdateNavMesh(uint32 mapId, uint32 diff) override
        {
            // about once a second: the grids within the visibility range of the map's players, each player's own grid first: made if
            // they are not (paths), noted as in use; about every 10 s the grids no player has had in range for a minute are forgotten
            int64 now = NowMs();
            int64 last, lastEviction;
            {
                std::lock_guard<std::mutex> guard(_gridsLock);
                last = _navAsked[mapId];
                lastEviction = _evicted[mapId];
                if (now - last >= 1000)
                    _navAsked[mapId] = now;
                if (now - lastEviction >= std::min<int64>(10000, g_keepMs.load() / 2))
                    _evicted[mapId] = now;
            }
            if (now - last >= 1000)
            {
                last = now;
                std::vector<PlayerView> views;
                {
                    std::lock_guard<std::mutex> guard(g_playersLock);
                    for (auto const& [guid, view] : g_players)
                        if (view.map == mapId)   // (a character that is loading is at the place where it will appear: its surroundings are kept)
                            views.push_back(view);
                }
                for (PlayerView const& view : views)
                {
                    GridCoord own = Acore::ComputeGridCoord(view.x, view.y);
                    GridCoord low = Acore::ComputeGridCoord(view.x + view.range, view.y + view.range);
                    GridCoord high = Acore::ComputeGridCoord(view.x - view.range, view.y - view.range);
                    std::vector<std::pair<uint32, uint32>> grids{ { own.x_coord, own.y_coord } };
                    for (uint32 gx = std::min(low.x_coord, high.x_coord); gx <= std::max(low.x_coord, high.x_coord); ++gx)
                        for (uint32 gy = std::min(low.y_coord, high.y_coord); gy <= std::max(low.y_coord, high.y_coord); ++gy)
                            if (gx != own.x_coord || gy != own.y_coord)
                                grids.emplace_back(gx, gy);
                    {
                        std::lock_guard<std::mutex> guard(_gridsLock);
                        for (auto const& [gx, gy] : grids)
                            _inUse[uint64(mapId) << 16 | gx << 8 | gy] = now;
                    }
                    if (_paths)
                        _nav.Want(mapId, grids);
                }
            }
            if (now - lastEviction >= std::min<int64>(10000, g_keepMs.load() / 2))
            {
                std::vector<std::pair<uint32, uint32>> stale;
                {
                    std::lock_guard<std::mutex> guard(_gridsLock);
                    for (auto grid = _inUse.begin(); grid != _inUse.end(); )
                    {
                        if (uint32(grid->first >> 16) == mapId && now - grid->second >= g_keepMs.load())
                        {
                            stale.emplace_back(uint32(grid->first >> 8) & 255, uint32(grid->first) & 255);
                            grid = _inUse.erase(grid);
                        }
                        else
                            ++grid;
                    }
                }
                for (auto const& [gx, gy] : stale)
                    _vmaps.Evict(mapId, gx, gy);
                if (_paths && !stale.empty())
                    _nav.Evict(mapId, stale);
            }
            if (_paths)
                _nav.Update(mapId, diff);
        }

    private:
        // ---- the map's models from the grids made from the game's files (VmapTiles), as the server's StaticMapTree answers from vmaps ----

        // the model instances of the grids the box between (x1, y1) and (x2, y2) touches, each once, when a player witnesses both ends and
        // all of those grids are made (the ones not made yet are made on the maker thread: until then the game answers)
        bool Local(Kind kind, uint32 mapId, float x1, float y1, float x2, float y2, std::vector<VMAP::ModelInstance const*>& models,
            std::vector<std::shared_ptr<VmapTile const>>& holders)
        {
            if (!Witnessed(mapId, x1, y1) || ((x1 != x2 || y1 != y2) && !Witnessed(mapId, x2, y2)))
                return false;
            GridCoord a = Acore::ComputeGridCoord(x1, y1), b = Acore::ComputeGridCoord(x2, y2);
            uint32 lowX = std::min(a.x_coord, b.x_coord), highX = std::max(a.x_coord, b.x_coord);
            uint32 lowY = std::min(a.y_coord, b.y_coord), highY = std::max(a.y_coord, b.y_coord);
            if (highX >= MAX_NUMBER_OF_GRIDS || highY >= MAX_NUMBER_OF_GRIDS || (highX - lowX) * (highY - lowY) > 16)
                return false;
            bool allReady = true;
            for (uint32 gx = lowX; gx <= highX; ++gx)
                for (uint32 gy = lowY; gy <= highY; ++gy)
                {
                    bool ready;
                    std::shared_ptr<VmapTile const> tile = _vmaps.TryGet(mapId, gx, gy, ready);
                    if (!ready)
                        allReady = false;
                    else if (tile)
                    {
                        holders.push_back(tile);
                        for (auto const& instance : tile->instances)
                            models.push_back(instance.get());
                    }
                }
            if (!allReady)
                return false;
            if (lowX != highX || lowY != highY)
            {
                std::sort(models.begin(), models.end());
                models.erase(std::unique(models.begin(), models.end()), models.end());
            }
            ++g_counts[kind].asked;
            ++g_counts[kind].local;
            return true;
        }

        static bool LocalLineOfSight(std::vector<VMAP::ModelInstance const*> const& models, float x1, float y1, float z1, float x2, float y2,
            float z2, VMAP::ModelIgnoreFlags ignoreFlags)
        {
            G3D::Vector3 const pos1 = VMAP::VMapMgr2::convertPositionToInternalRep(x1, y1, z1);
            G3D::Vector3 const pos2 = VMAP::VMapMgr2::convertPositionToInternalRep(x2, y2, z2);
            float maxDist = (pos2 - pos1).magnitude();
            if (maxDist == std::numeric_limits<float>::max() || !std::isfinite(maxDist))
                return false;
            if (maxDist < 1e-10f)
                return true;
            G3D::Ray ray = G3D::Ray::fromOriginAndDirection(pos1, (pos2 - pos1) / maxDist);
            for (VMAP::ModelInstance const* model : models)
            {
                float distance = maxDist;
                if (model->intersectRay(ray, distance, true, ignoreFlags))
                    return false;
            }
            return true;
        }

        // the nearest hit along the ray within maxDist (shortened to it): true when there is one
        static bool LocalNearestHit(std::vector<VMAP::ModelInstance const*> const& models, G3D::Ray const& ray, float& maxDist)
        {
            bool hit = false;
            for (VMAP::ModelInstance const* model : models)
                if (model->intersectRay(ray, maxDist, false, VMAP::ModelIgnoreFlags::Nothing))
                    hit = true;
            return hit;
        }

        static bool LocalObjectHitPos(std::vector<VMAP::ModelInstance const*> const& models, float x1, float y1, float z1, float x2, float y2,
            float z2, float& rx, float& ry, float& rz, float modifyDist)
        {
            G3D::Vector3 const pos1 = VMAP::VMapMgr2::convertPositionToInternalRep(x1, y1, z1);
            G3D::Vector3 const pos2 = VMAP::VMapMgr2::convertPositionToInternalRep(x2, y2, z2);
            G3D::Vector3 result = pos2;
            bool hit = false;
            float maxDist = (pos2 - pos1).magnitude();
            if (maxDist >= 1e-10f && std::isfinite(maxDist))
            {
                G3D::Vector3 dir = (pos2 - pos1) / maxDist;
                float distance = maxDist;
                if (LocalNearestHit(models, G3D::Ray(pos1, dir), distance))
                {
                    result = pos1 + dir * distance;
                    if (modifyDist < 0)
                        result = (result - pos1).magnitude() > -modifyDist ? result + dir * modifyDist : pos1;
                    else
                        result = result + dir * modifyDist;
                    hit = true;
                }
            }
            result = VMAP::VMapMgr2::convertPositionToInternalRep(result.x, result.y, result.z);   // (back: the conversion is its own inverse)
            rx = result.x;
            ry = result.y;
            rz = result.z;
            return hit;
        }

        static float LocalHeight(std::vector<VMAP::ModelInstance const*> const& models, float x, float y, float z, float maxSearchDist)
        {
            G3D::Vector3 const pos = VMAP::VMapMgr2::convertPositionToInternalRep(x, y, z);
            float distance = maxSearchDist;
            if (!LocalNearestHit(models, G3D::Ray(pos, G3D::Vector3(0, 0, -1)), distance))
                return VMAP_INVALID_HEIGHT_VALUE;
            return pos.z - distance;
        }

        // as StaticVMapCollisionData::GetAreaAndLiquidData with the static tree (the caller applies the map's vmap disables)
        static bool LocalAreaAndLiquidData(std::vector<VMAP::ModelInstance const*> const& models, float x, float y, float z,
            Optional<uint8> reqLiquidType, VMAP::AreaAndLiquidData& data)
        {
            G3D::Vector3 const pos = VMAP::VMapMgr2::convertPositionToInternalRep(x, y, z);
            VMAP::LocationInfo info;
            bool found = false;
            for (VMAP::ModelInstance const* model : models)
                if (model->GetLocationInfo(pos, info))
                    found = true;
            if (!found)
                return false;
            data.floorZ = info.ground_Z;
            uint32 liquidType = info.hitModel->GetLiquidType();   // (LiquidType.dbc)
            float liquidLevel;
            if (!reqLiquidType || (GetLiquidFlags(liquidType) & *reqLiquidType))
                if (info.hitInstance->GetLiquidLevel(pos, info, liquidLevel))
                    data.liquidInfo.emplace(liquidType, liquidLevel);
            data.areaInfo.emplace(info.hitModel->GetWmoID(), info.hitInstance->adtId, info.rootId, info.hitModel->GetMogpFlags(),
                info.hitInstance->ID);
            return true;
        }

        // whether to compare this answer with the game's (WoWController.World.Compare)
        static bool Compare()
        {
            uint32 every = g_compareEvery.load();
            return every && g_compareTick.fetch_add(1) % every == 0;
        }

        static void Compared(Kind kind, bool same, uint32 mapId, float x1, float y1, float z1, float x2, float y2, float z2,
            std::string const& ours, std::string const& game)
        {
            ++(same ? g_counts[kind].agree : g_counts[kind].disagree);
            if (!same && g_mismatchesLogged.fetch_add(1) < 30)
                LOG_INFO("module", "{} differs from the game's: map {} {} {} {} -> {} {} {}: from the files {}, the game {}",
                    KindNames[kind], mapId, x1, y1, z1, x2, y2, z2, ours, game);
        }

        static WoWWorldQuery Query(uint32 op, uint32 mapId, uint32 flags, float x, float y, float z)
        {
            WoWWorldQuery q;
            q.op = op;
            q.map = mapId;
            q.flags = flags;
            q.p[0] = x;
            q.p[1] = y;
            q.p[2] = z;
            return q;
        }

        static WoWWorldQuery Trace(uint32 mapId, uint32 flags, float x1, float y1, float z1, float x2, float y2, float z2)
        {
            WoWWorldQuery q = Query(WoWWorldQuery::Trace, mapId, flags, x1, y1, z1);
            q.p[3] = x2;
            q.p[4] = y2;
            q.p[5] = z2;
            return q;
        }

        // a player in the world on the map sees the position (within the map's visibility range)
        static bool Witnessed(uint32 mapId, float x, float y)
        {
            std::lock_guard<std::mutex> guard(g_playersLock);
            for (auto const& [guid, view] : g_players)
            {
                if (view.loading || view.map != mapId)
                    continue;
                float dx = x - view.x, dy = y - view.y;
                if (dx * dx + dy * dy <= view.range * view.range)
                    return true;
            }
            return false;
        }

        // the terrain of a position a player witnesses (made from the game's ADT the first time); nullptr: no data
        std::shared_ptr<GridTerrainData> Terrain(Kind kind, uint32 mapId, float x, float y)
        {
            KindCounts& counts = g_counts[kind];
            ++counts.asked;
            bool witnessed = Witnessed(mapId, x, y);
            if (g_sampleTick.fetch_add(1) % SampleEvery == 0)
                SampleCaller(kind, witnessed);
            if (!witnessed)
                return nullptr;
            ++counts.sent;
            auto const before = std::chrono::steady_clock::now();
            std::shared_ptr<GridTerrainData> terrain = _terrain.Get(mapId, x, y);
            uint64 us = uint64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before).count());
            counts.waitUs += us;
            uint64 max = counts.maxWaitUs.load();
            while (us > max && !counts.maxWaitUs.compare_exchange_weak(max, us)) { }
            if (!terrain)
                ++counts.unknown;
            return terrain;
        }

        // the answers: only questions about what a player witnesses go to the game, the others are unknown (no data) at once
        void Ask(Kind kind, WoWWorldQuery* queries, std::size_t count)
        {
            KindCounts& counts = g_counts[kind];
            ++counts.asked;
            for (std::size_t i = 0; i < count; ++i)
                queries[i].known = 0;
            WoWWorldQuery const& first = queries[0];
            bool witnessed = Witnessed(first.map, first.p[0], first.p[1]) &&
                (first.op != WoWWorldQuery::Trace || Witnessed(first.map, first.p[3], first.p[4]));
            if (g_sampleTick.fetch_add(1) % SampleEvery == 0)
                SampleCaller(kind, witnessed);
            if (!witnessed)
                return;

            ++counts.sent;
            auto const before = std::chrono::steady_clock::now();
            if (!_process->WorldQueries(queries, count))
                for (std::size_t i = 0; i < count; ++i)
                    queries[i].known = 0;
            uint64 us = uint64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before).count());
            counts.waitUs += us;
            uint64 max = counts.maxWaitUs.load();
            while (us > max && !counts.maxWaitUs.compare_exchange_weak(max, us)) { }
            for (std::size_t i = 0; i < count; ++i)
                if (!queries[i].known)
                {
                    ++counts.unknown;
                    break;
                }
        }

        WoWProcess* _process;
        AdtTerrain _terrain;
        VmapTiles _vmaps;
        NavTiles _nav;
        bool _paths;   // the navigation meshes are made (WoWController.Paths)
        std::mutex _gridsLock;                         // the three below (the maps' updates may run on several threads)
        std::unordered_map<uint32, int64> _navAsked;   // when the grids around the players were last asked for, per map
        std::unordered_map<uint32, int64> _evicted;    // when the grids no player has had in range lately were last forgotten, per map
        std::unordered_map<uint64, int64> _inUse;      // when each grid (map << 16 | gx << 8 | gy) was last in a player's range
    };
}

std::shared_ptr<MapDataProvider> CreateWoWMapData(WoWProcess* process, bool paths)
{
    g_compareEvery = sConfigMgr->GetOption<uint32>("WoWController.World.Compare", 0);
    g_keepMs = int64(sConfigMgr->GetOption<uint32>("WoWController.World.KeepSeconds", 60)) * 1000;
    return std::make_shared<WoWMapData>(process, paths);
}

bool NoteWoWMapDataPlayer(Player* player, bool gone)
{
    std::lock_guard<std::mutex> guard(g_playersLock);
    if (gone)
    {
        g_players.erase(player->GetGUID().GetRawValue());
        return false;
    }
    PlayerView& view = g_players[player->GetGUID().GetRawValue()];
    bool wasLoading = view.loading;
    float oldX = view.x, oldY = view.y;
    uint32 oldMap = view.map;
    view.map = player->GetMapId();
    view.x = player->GetPositionX();
    view.y = player->GetPositionY();
    if (player->IsBeingTeleported())
    {
        // a character on its way: it counts as being where it will appear
        WorldLocation const& destination = player->GetTeleportDest();
        view.map = destination.GetMapId();
        view.x = destination.GetPositionX();
        view.y = destination.GetPositionY();
    }
    view.range = player->IsInWorld() ? player->GetMap()->GetVisibilityRange() : 150.0f;
    view.loading = !player->IsInWorld() || player->IsBeingTeleportedFar() || (player->GetSession() && player->GetSession()->PlayerLoading());
    // done loading, or a jump (a teleport on the same map: what was worked out at the new position was outside the old view)
    float dx = view.x - oldX, dy = view.y - oldY;
    bool jumped = !view.loading && (view.map != oldMap || dx * dx + dy * dy > 50.0f * 50.0f);
    return (wasLoading && !view.loading) || jumped;
}

void NoteWoWMapDataGameObject(GameObject const* go, bool removed)
{
    GameobjectTypes type = go->GetGoType();
    if (type != GAMEOBJECT_TYPE_DOOR && type != GAMEOBJECT_TYPE_DESTRUCTIBLE_BUILDING && type != GAMEOBJECT_TYPE_TRAPDOOR)
        return;
    std::lock_guard<std::mutex> guard(g_objectsLock);
    auto& objects = g_objects[go->GetMapId()];
    if (removed)
    {
        objects.erase(go->GetGUID().GetRawValue());
        return;
    }
    // (its size: the farthest corner of its display bounds, at least 2 yd)
    float radius = 2.0f;
    if (GameObjectDisplayInfoEntry const* info = sGameObjectDisplayInfoStore.LookupEntry(go->GetDisplayId()))
    {
        float scale = go->GetObjectScale() > 0.0f ? go->GetObjectScale() : 1.0f;
        float extentX = std::max(std::fabs(info->minX), std::fabs(info->maxX)), extentY = std::max(std::fabs(info->minY), std::fabs(info->maxY));
        radius = std::max(radius, std::sqrt(extentX * extentX + extentY * extentY) * scale);
    }
    objects[go->GetGUID().GetRawValue()] = ObjectView{ go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), radius };
}

void NoteWoWMapDataTeleport(Player* player, uint32 mapId)
{
    std::lock_guard<std::mutex> guard(g_playersLock);
    auto found = g_players.find(player->GetGUID().GetRawValue());
    if (found != g_players.end() && found->second.map != mapId)
        found->second.loading = true;   // (a far teleport: the game shows its loading screen until the new map is there)
}

std::vector<std::string> DescribeWoWMapDataUse()
{
    std::vector<std::string> lines;
    int64 start = g_windowStartMs.load();
    double seconds = start ? (NowMs() - start) / 1000.0 : 0.0;
    uint64 asked = 0, sent = 0;
    std::ostringstream kinds;
    for (int k = 0; k < KindCount; ++k)
    {
        KindCounts const& c = g_counts[k];
        uint64 kAsked = c.asked.load(), kSent = c.sent.load(), kWait = c.waitUs.load(), kMax = c.maxWaitUs.load(), kUnknown = c.unknown.load();
        asked += kAsked;
        sent += kSent;
        if (!kSent)
            continue;
        kinds << "; " << KindNames[k] << " " << kSent << (KindAsksGame[k] ? " asked of the game" : " near the player") << " (" << kUnknown
              << " unknown, " << std::fixed << std::setprecision(2) << (kWait / 1000.0 / kSent) << " ms each, " << (kMax / 1000.0) << " ms at most)";
    }
    std::ostringstream head;
    head << "World questions of the server (server side, last " << std::fixed << std::setprecision(0) << seconds << " s): " << asked << " asked, "
         << sent << " near the player" << kinds.str();
    lines.push_back(head.str());
    return lines;
}

void LogWoWMapDataUse(uint32 seconds)
{
    g_windowStartMs = NowMs();
    std::ostringstream text;
    uint64 asked = 0, sent = 0;
    for (int k = 0; k < KindCount; ++k)
    {
        KindCounts& c = g_counts[k];
        uint64 kAsked = c.asked.exchange(0), kSent = c.sent.exchange(0), kUnknown = c.unknown.exchange(0), kWait = c.waitUs.exchange(0),
            kMax = c.maxWaitUs.exchange(0);
        if (!kAsked)
            continue;
        asked += kAsked;
        sent += kSent;
        text << "; " << KindNames[k] << " " << kAsked << " (" << kSent << (KindAsksGame[k] ? " asked of the game" : " near the player");
        if (kSent)
            text << ", " << kUnknown << " unknown there, took " << (kWait / kSent) << " us each, " << (kMax / 1000) << " ms at most, "
                 << (kWait / 1000) << " ms in all";
        uint64 kLocal = c.local.exchange(0), kAgree = c.agree.exchange(0), kDisagree = c.disagree.exchange(0);
        if (kLocal)
            text << ", " << kLocal << " from the game's files";
        if (kAgree || kDisagree)
            text << ", compared: " << kAgree << " same, " << kDisagree << " different";
        text << ")";
    }
    if (!asked)
        return;
    LOG_INFO("module", "World questions in {} s: {} ({} near the player){}", seconds, asked, sent, text.str());
    LogAdtTerrainUse();
    for (std::string const& line : { DescribeVmapTilesUse(true), DescribeNavTilesUse(true) })
        if (!line.empty())
            LOG_INFO("module", "{}", line);

    std::vector<std::pair<std::string, uint64>> callers;
    {
        std::lock_guard<std::mutex> guard(g_callersLock);
        callers.assign(g_callers.begin(), g_callers.end());
        g_callers.clear();
    }
    std::sort(callers.begin(), callers.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < callers.size() && i < 12; ++i)
        LOG_INFO("module", "  asked by (1 in {} sampled) {}x {}", SampleEvery, callers[i].second, callers[i].first);
}
