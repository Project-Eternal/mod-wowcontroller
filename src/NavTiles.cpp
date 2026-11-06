/*
 * The navigation meshes of the game's maps, per grid, as a Detour tile cache (NavTiles.h).
 * Making a grid (on the navigation thread): its terrain and the terrain edges of its neighbours (.map files of AdtTerrain) and its models
 * (VmapTiles) are triangulated by the mmaps generator's TerrainBuilder; each of its 25 x 25 tiles (80 cells of 0.2667 yd, the generator's
 * settings) is rasterized as the generator does (walkable slope, ledges, low spans, liquids as their own areas) and stored as compressed
 * height layers. The map's update adds them to the map's tile cache, which builds the mesh tiles from them and from the obstacles: game
 * objects that block while closed (doors), boxes of their display bounds. The polygons' flags are their areas (NAV_GROUND, NAV_WATER...),
 * as the generator sets them, for the server's path filter.
 */
#include "NavTiles.h"
#include "AdtTerrain.h"
#include "DBCStores.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "GameObject.h"
#include "GridDefines.h"
#include "Log.h"
#include "MapDefines.h"
#include "ModelInstance.h"
#include "Recast.h"
#include "VmapTiles.h"
#include "navigation/DetourTileCache.h"
#include "navigation/DetourTileCacheBuilder.h"
#include "navigation/TerrainBuilder.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <zlib.h>

namespace
{
    // the mmaps generator's settings (mmaps-config.yaml, Config.h): 2000 cells per grid edge, tiles of 80, in cells: agent height 6,
    // climb 4, radius 2; walkable slope 60 degrees, contour simplification 1.8
    constexpr float GridSize = SIZE_OF_GRIDS;
    constexpr int CellsPerGrid = 2000;
    constexpr int TileCells = 80;
    constexpr int TilesPerGrid = CellsPerGrid / TileCells;
    constexpr float CellSize = GridSize / CellsPerGrid;
    constexpr float TileSize = CellSize * TileCells;
    constexpr int WalkableHeight = 6, WalkableClimb = 4, WalkableRadius = 2;
    constexpr float WalkableSlope = 60.0f, MaxSimplificationError = 1.8f;
    constexpr int BorderSize = WalkableRadius + 3;
    constexpr int MaxLayers = 32;            // height layers per tile
    constexpr int MaxMeshTiles = 1 << 16;    // tiles in a map's mesh (about 100 grids)

    // the tile cache keeps its layers compressed with zlib (fastest level: the layers compress well)
    struct ZlibCompression : dtTileCacheCompressor
    {
        int maxCompressedSize(int bufferSize) override { return int(compressBound(uLong(bufferSize))); }
        dtStatus compress(unsigned char const* buffer, int bufferSize, unsigned char* compressed, int maxCompressedSize, int* compressedSize) override
        {
            uLongf size = uLongf(maxCompressedSize);
            if (compress2(compressed, &size, buffer, uLong(bufferSize), 1) != Z_OK)
                return DT_FAILURE;
            *compressedSize = int(size);
            return DT_SUCCESS;
        }
        dtStatus decompress(unsigned char const* compressed, int compressedSize, unsigned char* buffer, int maxBufferSize, int* bufferSize) override
        {
            uLongf size = uLongf(maxBufferSize);
            if (uncompress(buffer, &size, compressed, uLong(compressedSize)) != Z_OK)
                return DT_FAILURE;
            *bufferSize = int(size);
            return DT_SUCCESS;
        }
    };

    // the polygons' flags are their areas, as the generator sets them (MapBuilder: flags = areas of walkable polygons)
    struct AreasAsFlags : dtTileCacheMeshProcess
    {
        void process(dtNavMeshCreateParams* params, unsigned char* polyAreas, unsigned short* polyFlags) override
        {
            for (int i = 0; i < params->polyCount; ++i)
                polyFlags[i] = polyAreas[i];
        }
    };

    // a grid made on the navigation thread: its tiles' layers (the tile cache's format), for the map's update to add
    struct MadeGrid
    {
        uint32 gx, gy;
        std::vector<std::vector<unsigned char>> layers;
    };

    // an obstacle to add (a box, the mesh's coordinates) or to remove, by the game object's guid
    struct ObstacleChange
    {
        uint64 guid;
        bool add;
        float bmin[3], bmax[3];
    };

    struct MapNavigation
    {
        std::shared_ptr<dtNavMesh> navMesh;
        std::unique_ptr<dtTileCache, void (*)(dtTileCache*)> tileCache{nullptr, dtFreeTileCache};
        dtTileCacheAlloc alloc;
        ZlibCompression compressor;
        AreasAsFlags meshProcess;
        std::unordered_set<uint32> requested;                  // grids asked for (made or being made)
        std::vector<MadeGrid> made;                            // made, not added yet (under g_lock)
        std::deque<ObstacleChange> obstacleChanges;            // (under g_lock)
        // the obstacles in the tile cache and their boxes (the map's update only): the tile cache applies an obstacle to the tiles it has
        // when the obstacle is added, so it is added again when tiles under it are added later
        struct Obstacle
        {
            dtObstacleRef ref;
            float bmin[3], bmax[3];
        };
        std::unordered_map<uint64, Obstacle> obstacles;
    };

    std::mutex g_lock;   // the maps, their requests and what waits for their update
    std::unordered_map<uint32, std::unique_ptr<MapNavigation>> g_maps;
    std::deque<std::array<uint32, 3>> g_queue;   // grids to make (map, gx, gy)
    std::unordered_set<uint64> g_making;         // the grid the navigation thread is making (map << 16 | gx << 8 | gy)
    std::condition_variable g_queueChanged;
    bool g_threadStarted = false;

    std::atomic<uint32> g_gridsMade{0}, g_tilesMade{0}, g_layersAdded{0}, g_buildFailures{0}, g_failuresLogged{0}, g_gridsEvicted{0};
    std::atomic<uint64> g_makeUs{0}, g_addUs{0}, g_layerBytes{0};

    // the map's navigation (made the first time); g_lock is held
    MapNavigation& Navigation(uint32 mapId)
    {
        std::unique_ptr<MapNavigation>& map = g_maps[mapId];
        if (map)
            return *map;
        map = std::make_unique<MapNavigation>();
        // (the mesh's coordinates are the mmaps': y, z, x; the world's west/north edge at 32 grids)
        dtNavMeshParams meshParams{};
        meshParams.orig[0] = -32.0f * GridSize;
        meshParams.orig[1] = 0.0f;
        meshParams.orig[2] = -32.0f * GridSize;
        meshParams.tileWidth = TileSize;
        meshParams.tileHeight = TileSize;
        meshParams.maxTiles = MaxMeshTiles;
        meshParams.maxPolys = 1 << 14;
        dtNavMesh* mesh = dtAllocNavMesh();
        if (!mesh || dtStatusFailed(mesh->init(&meshParams)))
        {
            LOG_ERROR("module", "The navigation mesh of map {} could not be made", mapId);
            dtFreeNavMesh(mesh);
            return *map;
        }
        map->navMesh = std::shared_ptr<dtNavMesh>(mesh, dtFreeNavMesh);

        dtTileCacheParams cacheParams{};
        rcVcopy(cacheParams.orig, meshParams.orig);
        cacheParams.cs = CellSize;
        cacheParams.ch = CellSize;
        cacheParams.width = TileCells;
        cacheParams.height = TileCells;
        cacheParams.walkableHeight = WalkableHeight * CellSize;
        cacheParams.walkableRadius = WalkableRadius * CellSize;
        cacheParams.walkableClimb = WalkableClimb * CellSize;
        cacheParams.maxSimplificationError = MaxSimplificationError;
        cacheParams.maxTiles = MaxMeshTiles * 4;
        cacheParams.maxObstacles = 1024;
        map->tileCache.reset(dtAllocTileCache());
        if (!map->tileCache || dtStatusFailed(map->tileCache->init(&cacheParams, &map->alloc, &map->compressor, &map->meshProcess)))
        {
            LOG_ERROR("module", "The navigation tile cache of map {} could not be made", mapId);
            map->tileCache.reset();
            map->navMesh.reset();
        }
        return *map;
    }

    // the layers of one tile (tile tx, ty of the mesh) from the grid's triangles, as the generator rasterizes its tiles
    void MakeTile(rcContext& context, WoWNav::MeshData& mesh, std::vector<int> const& solidTris, std::vector<int> const& liquidTris, int tx, int ty,
        float minHeight, float maxHeight, std::vector<std::vector<unsigned char>>& out)
    {
        rcConfig config{};
        config.cs = CellSize;
        config.ch = CellSize;
        config.walkableSlopeAngle = WalkableSlope;
        config.walkableHeight = WalkableHeight;
        config.walkableClimb = WalkableClimb;
        config.walkableRadius = WalkableRadius;
        config.maxEdgeLen = TileCells + 1;
        config.maxSimplificationError = MaxSimplificationError;
        config.minRegionArea = rcSqr(60);
        config.mergeRegionArea = rcSqr(50);
        config.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
        config.tileSize = TileCells;
        config.borderSize = BorderSize;
        config.width = TileCells + BorderSize * 2;
        config.height = TileCells + BorderSize * 2;
        config.bmin[0] = -32.0f * GridSize + tx * TileSize - BorderSize * CellSize;
        config.bmin[1] = minHeight;
        config.bmin[2] = -32.0f * GridSize + ty * TileSize - BorderSize * CellSize;
        config.bmax[0] = -32.0f * GridSize + (tx + 1) * TileSize + BorderSize * CellSize;
        config.bmax[1] = maxHeight;
        config.bmax[2] = -32.0f * GridSize + (ty + 1) * TileSize + BorderSize * CellSize;

        // (the triangles that reach into the tile: the grid's vertices, the tile's triangles' indices)
        float* verts = mesh.solidVerts.getCArray();
        int vertCount = mesh.solidVerts.size() / 3;
        std::vector<int> tris, liquids;
        std::vector<unsigned char> liquidTypes;
        for (int t : solidTris)
            tris.insert(tris.end(), { mesh.solidTris[t * 3], mesh.solidTris[t * 3 + 1], mesh.solidTris[t * 3 + 2] });
        for (int t : liquidTris)
        {
            liquids.insert(liquids.end(), { mesh.liquidTris[t * 3], mesh.liquidTris[t * 3 + 1], mesh.liquidTris[t * 3 + 2] });
            liquidTypes.push_back(mesh.liquidType[t]);
        }
        int triCount = int(tris.size() / 3);
        float* liquidVerts = mesh.liquidVerts.getCArray();
        int liquidVertCount = mesh.liquidVerts.size() / 3;
        int liquidTriCount = int(liquids.size() / 3);

        rcHeightfield* solid = rcAllocHeightfield();
        rcCompactHeightfield* chf = nullptr;
        rcHeightfieldLayerSet* layers = nullptr;
        if (solid && rcCreateHeightfield(&context, *solid, config.width, config.height, config.bmin, config.bmax, config.cs, config.ch))
        {
            std::vector<unsigned char> triFlags(std::max(triCount, 1), NAV_GROUND);
            rcClearUnwalkableTriangles(&context, config.walkableSlopeAngle, verts, vertCount, tris.data(), triCount, triFlags.data());
            rcRasterizeTriangles(&context, verts, vertCount, tris.data(), triFlags.data(), triCount, *solid, config.walkableClimb);
            rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb, *solid);
            rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb, *solid);
            rcFilterWalkableLowHeightSpans(&context, config.walkableHeight, *solid);
            if (liquidTriCount)
                rcRasterizeTriangles(&context, liquidVerts, liquidVertCount, liquids.data(), liquidTypes.data(), liquidTriCount, *solid, config.walkableClimb);
            chf = rcAllocCompactHeightfield();
            layers = rcAllocHeightfieldLayerSet();
            if (chf && layers && rcBuildCompactHeightfield(&context, config.walkableHeight, config.walkableClimb, *solid, *chf) &&
                rcErodeWalkableArea(&context, config.walkableRadius, *chf) &&
                rcBuildHeightfieldLayers(&context, *chf, config.borderSize, config.walkableHeight, *layers))
            {
                for (int i = 0; i < std::min(layers->nlayers, MaxLayers); ++i)
                {
                    rcHeightfieldLayer const& layer = layers->layers[i];
                    dtTileCacheLayerHeader header{};
                    header.magic = DT_TILECACHE_MAGIC;
                    header.version = DT_TILECACHE_VERSION;
                    header.tx = tx;
                    header.ty = ty;
                    header.tlayer = i;
                    dtVcopy(header.bmin, layer.bmin);
                    dtVcopy(header.bmax, layer.bmax);
                    header.width = (unsigned char)layer.width;
                    header.height = (unsigned char)layer.height;
                    header.minx = (unsigned char)layer.minx;
                    header.maxx = (unsigned char)layer.maxx;
                    header.miny = (unsigned char)layer.miny;
                    header.maxy = (unsigned char)layer.maxy;
                    header.hmin = (unsigned short)layer.hmin;
                    header.hmax = (unsigned short)layer.hmax;
                    ZlibCompression compressor;
                    unsigned char* data = nullptr;
                    int size = 0;
                    if (dtStatusSucceed(dtBuildTileCacheLayer(&compressor, &header, layer.heights, layer.areas, layer.cons, &data, &size)))
                    {
                        out.emplace_back(data, data + size);
                        g_layerBytes += size;
                    }
                    dtFree(data);
                }
            }
        }
        rcFreeHeightfieldLayerSet(layers);
        rcFreeCompactHeightfield(chf);
        rcFreeHeightField(solid);
        ++g_tilesMade;
    }

    // the grid's triangles and its tiles' layers (the navigation thread)
    MadeGrid MakeGrid(AdtTerrain& terrain, VmapTiles& vmaps, uint32 mapId, uint32 gx, uint32 gy)
    {
        MadeGrid made{ gx, gy, {} };
        // the terrain of the grid and of its neighbours (TerrainBuilder reads their edges from the .map files AdtTerrain keeps)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
            {
                int x = int(gx) + dx, y = int(gy) + dy;
                if ((dx && dy) || x < 0 || y < 0 || x >= MAX_NUMBER_OF_GRIDS || y >= MAX_NUMBER_OF_GRIDS)
                    continue;
                terrain.Get(mapId, (32.0f - x - 0.5f) * GridSize, (32.0f - y - 0.5f) * GridSize);
            }
        std::shared_ptr<VmapTile const> models = vmaps.Get(mapId, gx, gy);

        // (the generator's tile (tileX, tileY) is the server's grid (gy, gx))
        WoWNav::TerrainBuilder builder(AdtTerrainMapsDirectory(), false);
        WoWNav::MeshData mesh;
        builder.loadMap(mapId, gy, gx, mesh);
        if (models)
        {
            std::vector<VMAP::ModelInstance const*> instances;
            for (auto const& instance : models->instances)
                instances.push_back(instance.get());
            builder.loadModels(instances, mesh);
        }
        WoWNav::TerrainBuilder::cleanVertices(mesh.solidVerts, mesh.solidTris);
        WoWNav::TerrainBuilder::cleanVertices(mesh.liquidVerts, mesh.liquidTris);
        if (!mesh.solidVerts.size() && !mesh.liquidVerts.size())
            return made;

        // the height range of the grid (the generator's getTileBounds: the vertices' bounds)
        G3D::Array<float> all;
        all.append(mesh.liquidVerts);
        all.append(mesh.solidVerts);
        float bmin[3], bmax[3];
        rcCalcBounds(all.getCArray(), all.size() / 3, bmin, bmax);

        rcContext context(false);
        // (the mesh's x is the world's y: grid gy; its z the world's x: grid gx; tiles count from the west/north edge)
        int firstX = (63 - int(gy)) * TilesPerGrid, firstY = (63 - int(gx)) * TilesPerGrid;

        // each triangle into the buckets of the tiles (with their borders) it reaches (as Recast's chunky meshes: a tile rasterizes
        // only its own triangles, not all of the grid's)
        std::vector<std::vector<int>> solidBuckets(TilesPerGrid * TilesPerGrid), liquidBuckets(TilesPerGrid * TilesPerGrid);
        auto bucket = [&](G3D::Array<float> const& v, G3D::Array<int> const& t, std::vector<std::vector<int>>& buckets)
        {
            float const origin = -32.0f * GridSize, border = BorderSize * CellSize;
            for (int i = 0; i < t.size() / 3; ++i)
            {
                float lowX = 1e30f, highX = -1e30f, lowZ = 1e30f, highZ = -1e30f;
                for (int k = 0; k < 3; ++k)
                {
                    float const* p = &v[t[i * 3 + k] * 3];
                    lowX = std::min(lowX, p[0]);
                    highX = std::max(highX, p[0]);
                    lowZ = std::min(lowZ, p[2]);
                    highZ = std::max(highZ, p[2]);
                }
                int x0 = std::max(int(std::floor((lowX - border - origin) / TileSize)) - firstX, 0);
                int x1 = std::min(int(std::floor((highX + border - origin) / TileSize)) - firstX, TilesPerGrid - 1);
                int z0 = std::max(int(std::floor((lowZ - border - origin) / TileSize)) - firstY, 0);
                int z1 = std::min(int(std::floor((highZ + border - origin) / TileSize)) - firstY, TilesPerGrid - 1);
                for (int z = z0; z <= z1; ++z)
                    for (int x = x0; x <= x1; ++x)
                        buckets[z * TilesPerGrid + x].push_back(i);
            }
        };
        bucket(mesh.solidVerts, mesh.solidTris, solidBuckets);
        bucket(mesh.liquidVerts, mesh.liquidTris, liquidBuckets);

        for (int y = 0; y < TilesPerGrid; ++y)
            for (int x = 0; x < TilesPerGrid; ++x)
            {
                std::vector<int> const& solids = solidBuckets[y * TilesPerGrid + x];
                std::vector<int> const& liquids = liquidBuckets[y * TilesPerGrid + x];
                if (!solids.empty() || !liquids.empty())
                    MakeTile(context, mesh, solids, liquids, firstX + x, firstY + y, bmin[1], bmax[1], made.layers);
            }
        return made;
    }

    void NavigationThread(AdtTerrain* terrain, VmapTiles* vmaps)
    {
        for (;;)
        {
            std::array<uint32, 3> next;
            {
                std::unique_lock<std::mutex> guard(g_lock);
                g_queueChanged.wait(guard, [] { return !g_queue.empty(); });
                next = g_queue.front();
                g_queue.pop_front();
                g_making.insert(uint64(next[0]) << 16 | next[1] << 8 | next[2]);
            }
            auto const started = std::chrono::steady_clock::now();
            MadeGrid made = MakeGrid(*terrain, *vmaps, next[0], next[1], next[2]);
            g_makeUs += uint64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
            ++g_gridsMade;
            std::lock_guard<std::mutex> guard(g_lock);
            Navigation(next[0]).made.push_back(std::move(made));
            g_making.erase(uint64(next[0]) << 16 | next[1] << 8 | next[2]);
        }
    }

    // the box of a game object's display (GameObjectDisplayInfo bounds, scaled, turned by its facing) in the mesh's coordinates (y, z, x)
    bool ObstacleBox(GameObject const* go, float* bmin, float* bmax)
    {
        GameObjectDisplayInfoEntry const* info = sGameObjectDisplayInfoStore.LookupEntry(go->GetDisplayId());
        if (!info || !(info->maxX > info->minX && info->maxY > info->minY && info->maxZ > info->minZ))
            return false;
        float scale = go->GetObjectScale() > 0.0f ? go->GetObjectScale() : 1.0f;
        float o = go->GetOrientation(), c = std::cos(o), s = std::sin(o);
        float lowX = 1e30f, lowY = 1e30f, highX = -1e30f, highY = -1e30f;
        for (int corner = 0; corner < 4; ++corner)
        {
            float lx = (corner & 1 ? info->maxX : info->minX) * scale, ly = (corner & 2 ? info->maxY : info->minY) * scale;
            float wx = go->GetPositionX() + c * lx - s * ly, wy = go->GetPositionY() + s * lx + c * ly;
            lowX = std::min(lowX, wx);
            highX = std::max(highX, wx);
            lowY = std::min(lowY, wy);
            highY = std::max(highY, wy);
        }
        bmin[0] = lowY;
        bmin[1] = go->GetPositionZ() + info->minZ * scale;
        bmin[2] = lowX;
        bmax[0] = highY;
        bmax[1] = go->GetPositionZ() + info->maxZ * scale;
        bmax[2] = highX;
        return true;
    }
}

std::shared_ptr<dtNavMesh> NavTiles::GetNavMesh(uint32 mapId)
{
    std::lock_guard<std::mutex> guard(g_lock);
    return Navigation(mapId).navMesh;
}

void NavTiles::Want(uint32 mapId, std::vector<std::pair<uint32, uint32>> const& grids)
{
    std::lock_guard<std::mutex> guard(g_lock);
    MapNavigation& map = Navigation(mapId);
    if (!map.tileCache)
        return;
    bool first = true, added = false;
    for (auto const& [gx, gy] : grids)
    {
        if (gx < MAX_NUMBER_OF_GRIDS && gy < MAX_NUMBER_OF_GRIDS && map.requested.insert(gx << 8 | gy).second)
        {
            // (the first grid asked for, the player's own, goes before what waits)
            if (first)
                g_queue.push_front({ mapId, gx, gy });
            else
                g_queue.push_back({ mapId, gx, gy });
            added = true;
        }
        first = false;
    }
    if (!added)
        return;
    if (!g_threadStarted)
    {
        g_threadStarted = true;
        std::thread(NavigationThread, &_terrain, &_vmaps).detach();
    }
    g_queueChanged.notify_one();
}

uint32 NavTiles::Evict(uint32 mapId, std::vector<std::pair<uint32, uint32>> const& grids)
{
    std::vector<std::pair<uint32, uint32>> gone;
    MapNavigation* map;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto found = g_maps.find(mapId);
        if (found == g_maps.end() || !found->second->tileCache)
            return 0;
        map = found->second.get();
        for (auto const& [gx, gy] : grids)
        {
            uint32 key = gx << 8 | gy;
            bool busy = g_making.count(uint64(mapId) << 16 | key) != 0;
            for (auto const& waiting : g_queue)
                busy = busy || (waiting[0] == mapId && waiting[1] == gx && waiting[2] == gy);
            for (MadeGrid const& made : map->made)
                busy = busy || (made.gx == gx && made.gy == gy);
            if (busy || !map->requested.erase(key))
                continue;
            gone.emplace_back(gx, gy);
        }
    }
    // (the map's own update: nobody else changes the mesh)
    dtTileCache* cache = map->tileCache.get();
    dtNavMesh* mesh = map->navMesh.get();
    for (auto const& [gx, gy] : gone)
    {
        int firstX = (63 - int(gy)) * TilesPerGrid, firstY = (63 - int(gx)) * TilesPerGrid;
        for (int ty = firstY; ty < firstY + TilesPerGrid; ++ty)
            for (int tx = firstX; tx < firstX + TilesPerGrid; ++tx)
            {
                dtCompressedTileRef refs[MaxLayers];
                int count = cache->getTilesAt(tx, ty, refs, MaxLayers);
                for (int i = 0; i < count; ++i)
                    cache->removeTile(refs[i], nullptr, nullptr);
                dtMeshTile const* tiles[MaxLayers];
                count = mesh->getTilesAt(tx, ty, tiles, MaxLayers);
                for (int i = 0; i < count; ++i)
                    mesh->removeTile(mesh->getTileRef(tiles[i]), nullptr, nullptr);
            }
    }
    g_gridsEvicted += uint32(gone.size());
    return uint32(gone.size());
}

void NavTiles::Update(uint32 mapId, uint32 diff)
{
    // (at most this many mesh tiles per update: the map's update waits for them)
    constexpr int TilesPerUpdate = 150;
    std::vector<MadeGrid> made;
    std::deque<ObstacleChange> changes;
    MapNavigation* map;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto found = g_maps.find(mapId);
        if (found == g_maps.end() || !found->second->tileCache)
            return;
        map = found->second.get();
        // a part of the made grids: whole tiles (a tile's layers are next to each other), the rest stays for the next updates
        int budget = TilesPerUpdate;
        while (!map->made.empty() && budget > 0)
        {
            MadeGrid& grid = map->made.front();
            MadeGrid part{ grid.gx, grid.gy, {} };
            std::size_t taken = 0;
            int lastTx = INT32_MIN, lastTy = INT32_MIN;
            while (taken < grid.layers.size())
            {
                dtTileCacheLayerHeader const* header = reinterpret_cast<dtTileCacheLayerHeader const*>(grid.layers[taken].data());
                if (header->tx != lastTx || header->ty != lastTy)
                {
                    if (budget == 0)
                        break;
                    --budget;
                    lastTx = header->tx;
                    lastTy = header->ty;
                }
                part.layers.push_back(std::move(grid.layers[taken++]));
            }
            grid.layers.erase(grid.layers.begin(), grid.layers.begin() + taken);
            if (grid.layers.empty())
                map->made.erase(map->made.begin());
            made.push_back(std::move(part));
        }
        changes.swap(map->obstacleChanges);
    }
    dtTileCache* cache = map->tileCache.get();
    dtNavMesh* mesh = map->navMesh.get();
    auto const started = std::chrono::steady_clock::now();
    float addedMin[3] = { 1e30f, 1e30f, 1e30f }, addedMax[3] = { -1e30f, -1e30f, -1e30f };
    for (MadeGrid& grid : made)
    {
        std::unordered_set<uint64> tiles;
        for (std::vector<unsigned char>& layer : grid.layers)
        {
            // (the tile cache owns the copy it is given)
            unsigned char* data = (unsigned char*)dtAlloc(int(layer.size()), DT_ALLOC_PERM);
            memcpy(data, layer.data(), layer.size());
            dtCompressedTileRef ref;
            if (dtStatusFailed(cache->addTile(data, int(layer.size()), DT_COMPRESSEDTILE_FREE_DATA, &ref)))
            {
                dtFree(data);
                continue;
            }
            dtTileCacheLayerHeader const* header = reinterpret_cast<dtTileCacheLayerHeader const*>(layer.data());
            tiles.insert(uint64(uint32(header->tx)) << 32 | uint32(header->ty));
            dtVmin(addedMin, header->bmin);
            dtVmax(addedMax, header->bmax);
            ++g_layersAdded;
        }
        for (uint64 tile : tiles)
        {
            dtStatus status = cache->buildNavMeshTilesAt(int(tile >> 32), int(uint32(tile)), mesh);
            if (dtStatusFailed(status))
            {
                ++g_buildFailures;
                if (g_failuresLogged++ < 10)
                    LOG_ERROR("module", "The navigation tile {} {} of map {} was not built (status {:x})", int(tile >> 32),
                        int(uint32(tile)), mapId, status);
            }
        }
    }
    // the obstacles over the tiles just added, again (see Obstacle)
    if (addedMin[0] <= addedMax[0])
        for (auto& [guid, obstacle] : map->obstacles)
            if (dtOverlapBounds(obstacle.bmin, obstacle.bmax, addedMin, addedMax))
            {
                cache->removeObstacle(obstacle.ref);
                if (dtStatusFailed(cache->addBoxObstacle(obstacle.bmin, obstacle.bmax, &obstacle.ref)))
                    obstacle.ref = 0;
            }
    for (ObstacleChange const& change : changes)
    {
        auto known = map->obstacles.find(change.guid);
        if (known != map->obstacles.end())
        {
            cache->removeObstacle(known->second.ref);
            map->obstacles.erase(known);
        }
        MapNavigation::Obstacle obstacle{};
        if (change.add && dtStatusSucceed(cache->addBoxObstacle(change.bmin, change.bmax, &obstacle.ref)))
        {
            dtVcopy(obstacle.bmin, change.bmin);
            dtVcopy(obstacle.bmax, change.bmax);
            map->obstacles[change.guid] = obstacle;
        }
    }
    // (the obstacles' tiles are rebuilt a few at a time)
    cache->update(diff / 1000.0f, mesh);
    if (!made.empty())
        g_addUs += uint64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
}

void NavTiles::DoorChanged(GameObject const* go, bool removed)
{
    if (go->GetGoType() != GAMEOBJECT_TYPE_DOOR)
        return;
    ObstacleChange change{ go->GetGUID().GetRawValue(), false, {}, {} };
    // (a door blocks while it is closed: GO_STATE_READY; GO_STATE_ACTIVE is open)
    change.add = !removed && go->GetGoState() == GO_STATE_READY && ObstacleBox(go, change.bmin, change.bmax);
    std::lock_guard<std::mutex> guard(g_lock);
    auto found = g_maps.find(go->GetMapId());
    if (found == g_maps.end())
        return;   // (no navigation on the map yet: its doors come again when added to the world after it exists)
    found->second->obstacleChanges.push_back(change);
}

std::string DescribeNavTilesUse(bool reset)
{
    uint32 grids = reset ? g_gridsMade.exchange(0) : g_gridsMade.load(), tiles = reset ? g_tilesMade.exchange(0) : g_tilesMade.load();
    uint32 layers = reset ? g_layersAdded.exchange(0) : g_layersAdded.load();
    uint64 makeUs = reset ? g_makeUs.exchange(0) : g_makeUs.load(), addUs = reset ? g_addUs.exchange(0) : g_addUs.load();
    uint64 bytes = g_layerBytes.load();
    if (!grids && !layers)
        return std::string();
    uint32 failures = reset ? g_buildFailures.exchange(0) : g_buildFailures.load();
    std::size_t obstacles = 0;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (auto const& [mapId, map] : g_maps)
            obstacles += map->obstacles.size();
    }
    uint32 evicted = reset ? g_gridsEvicted.exchange(0) : g_gridsEvicted.load();
    std::ostringstream text;
    if (failures)
        text << failures << " navigation tiles not built; ";
    text << "navigation grids made: " << grids << " (" << tiles << " tiles, " << (grids ? makeUs / 1000 / grids : 0) << " ms each), "
         << layers << " layers added to the meshes in " << addUs / 1000 << " ms; " << bytes / 1024 << " KB of layers in all; " << obstacles
         << " closed doors as obstacles; " << evicted << " grids forgotten";
    return text.str();
}
