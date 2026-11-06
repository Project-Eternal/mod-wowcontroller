/*
 * The terrain of the game's maps, made from the game's own ADT files on first use: the ADT of a grid (World\Maps\<map>\<map>_<y>_<x>.adt,
 * read as the game reads it) is converted by AzerothCore's map extractor code (terrain/AdtConvert.cpp) into the server's .map format and
 * loaded by the server's own GridTerrainData, so its answers are the server's own. The tiles are kept (the least recently used ones go when
 * there are more than MaxTiles).
 */
#include "AdtTerrain.h"
#include "GridDefines.h"
#include "Log.h"
#include "StringFormat.h"
#include "terrain/AdtConvert.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr std::size_t MaxTiles = 64;

    struct Tile
    {
        std::shared_ptr<GridTerrainData> terrain;   // nullptr: none
        uint64 used = 0;
        int64 retryAt = 0;                          // 0: the answer is final; else the game did not answer in time, and it is asked again from then on
        uint32 tries = 0;
    };

    int64 NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    std::mutex g_tilesLock;                         // the tiles
    std::unordered_map<uint32, Tile> g_tiles;
    uint64 g_useClock = 0;
    std::mutex g_loadLock;                          // one conversion at a time (the extractor's work data is global)

    std::atomic<uint32> g_made{0}, g_missing{0};
    std::atomic<uint64> g_adtBytes{0}, g_makeUs{0};

    uint32 Key(uint32 mapId, uint32 gx, uint32 gy) { return mapId << 12 | gx << 6 | gy; }

    // reads and converts the ADT of grid (gx, gy) and loads it as the server would load maps/MMMXXYY.map; g_loadLock is held
    std::shared_ptr<GridTerrainData> Make(WoWProcess* process, uint32 mapId, uint32 gx, uint32 gy)
    {
        std::string directory = GameMapDirectory(process, mapId);
        if (directory.empty())
            return nullptr;
        // (the extractor writes maps/MMM<y><x>.map from <map>_<x>_<y>.adt: grid (gx, gy) of the server is <map>_<gy>_<gx>.adt)
        std::string name = "World\\Maps\\" + directory + "\\" + directory + "_" + std::to_string(gy) + "_" + std::to_string(gx) + ".adt";
        std::vector<uint8> adt, map;
        if (!process->ReadFile(name, adt) || !WoWTerrain::ConvertADT(name, adt, map, 12340))
            return nullptr;
        g_adtBytes += adt.size();

        // (kept under the server's name: the navigation meshes read the grids' .map files, AdtTerrainMapsDirectory)
        std::filesystem::path file = std::filesystem::path(AdtTerrainMapsDirectory()) / Acore::StringFormat("{:03}{:02}{:02}.map", mapId, gx, gy);
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<char const*>(map.data()), std::streamsize(map.size()));
            if (!out)
            {
                LOG_ERROR("module", "{} could not be written: no terrain", file.string());
                return nullptr;
            }
        }
        auto terrain = std::make_shared<GridTerrainData>();
        TerrainMapDataReadResult result = terrain->Load(file.string());
        if (result != TerrainMapDataReadResult::Success)
        {
            LOG_ERROR("module", "The terrain made from {} did not load ({})", name, uint32(result));
            return nullptr;
        }
        return terrain;
    }
}

std::string AdtTerrainMapsDirectory()
{
    static std::string const directory = []
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() / "wowcontroller-maps";
        std::error_code ignored;
        std::filesystem::create_directories(path, ignored);
        return path.string();
    }();
    return directory;
}

namespace
{
    std::mutex g_directoriesLock;
    std::unordered_map<uint32, std::string> g_directories;
    bool g_directoriesRead = false;   // (Map.dbc was taken, from the DBC loader's read or from the fallback read)

    // field 1 of every record of Map.dbc (the folder name in the game); the lock is held
    void ParseMapDbc(std::vector<uint8> const& dbc)
    {
        if (dbc.size() < 20 || memcmp(dbc.data(), "WDBC", 4) != 0)
            return;
        uint32 records, fields, recordSize, stringSize;
        memcpy(&records, &dbc[4], 4);
        memcpy(&fields, &dbc[8], 4);
        memcpy(&recordSize, &dbc[12], 4);
        memcpy(&stringSize, &dbc[16], 4);
        std::size_t strings = 20 + std::size_t(records) * recordSize;
        if (fields >= 2 && strings + stringSize <= dbc.size())
            for (uint32 i = 0; i < records; ++i)
            {
                uint32 id, name;
                memcpy(&id, &dbc[20 + std::size_t(i) * recordSize], 4);
                memcpy(&name, &dbc[20 + std::size_t(i) * recordSize + 4], 4);
                if (name < stringSize)
                    g_directories[id] = reinterpret_cast<char const*>(&dbc[strings + name]);
            }
    }
}

void NoteGameMapDbc(std::vector<uint8> const& dbc)
{
    std::lock_guard<std::mutex> guard(g_directoriesLock);
    ParseMapDbc(dbc);
    if (!g_directories.empty())
        g_directoriesRead = true;
}

std::string GameMapDirectory(WoWProcess* process, uint32 mapId)
{
    std::lock_guard<std::mutex> guard(g_directoriesLock);
    if (!g_directoriesRead)
    {
        // (the DBC loader did not take Map.dbc from the game, WoWController.Dbc = 0: read it here, once)
        g_directoriesRead = true;
        std::vector<uint8> dbc;
        if (process->ReadFile("DBFilesClient\\Map.dbc", dbc))
            ParseMapDbc(dbc);
        if (g_directories.empty())
            LOG_ERROR("module", "The game's Map.dbc could not be read: no terrain from the game");
    }
    auto& directories = g_directories;
    auto found = directories.find(mapId);
    return found != directories.end() ? found->second : std::string();
}

std::shared_ptr<GridTerrainData> AdtTerrain::Get(uint32 mapId, float x, float y)
{
    GridCoord grid = Acore::ComputeGridCoord(x, y);
    if (grid.x_coord >= MAX_NUMBER_OF_GRIDS || grid.y_coord >= MAX_NUMBER_OF_GRIDS)
        return nullptr;
    uint32 key = Key(mapId, grid.x_coord, grid.y_coord);
    {
        std::lock_guard<std::mutex> guard(g_tilesLock);
        auto found = g_tiles.find(key);
        if (found != g_tiles.end() && (!found->second.retryAt || NowMs() < found->second.retryAt))
        {
            found->second.used = ++g_useClock;
            return found->second.terrain;
        }
    }

    std::lock_guard<std::mutex> loading(g_loadLock);
    uint32 tries = 0;
    {
        std::lock_guard<std::mutex> guard(g_tilesLock);   // (another thread may have made it meanwhile)
        auto found = g_tiles.find(key);
        if (found != g_tiles.end())
        {
            if (!found->second.retryAt || NowMs() < found->second.retryAt)
                return found->second.terrain;
            tries = found->second.tries;
        }
    }
    auto const started = std::chrono::steady_clock::now();
    uint32 timeouts = _process->Timeouts();
    std::shared_ptr<GridTerrainData> terrain = Make(_process, mapId, grid.x_coord, grid.y_coord);
    bool timedOut = !terrain && _process->Timeouts() != timeouts;
    g_makeUs += uint64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    ++(terrain ? g_made : g_missing);
    if (timedOut)
    {
        // the game did not answer in time (it is busy, perhaps waiting for this very server): no terrain for now, and it is asked again later with a growing
        // delay; the server goes on meanwhile (creatures there move in a simpler way)
        std::lock_guard<std::mutex> guard(g_tilesLock);
        g_tiles[key] = Tile{ nullptr, ++g_useClock, NowMs() + std::min<int64>(60000, int64(2000) << std::min<uint32>(tries, 5)), tries + 1 };
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(g_tilesLock);
    if (g_tiles.size() >= MaxTiles)
    {
        auto oldest = g_tiles.begin();
        for (auto i = g_tiles.begin(); i != g_tiles.end(); ++i)
            if (i->second.used < oldest->second.used)
                oldest = i;
        // (its .map file goes too: the navigation reads the files of the grids it makes right after getting them from here)
        uint32 key = oldest->first;
        std::error_code ignored;
        std::filesystem::remove(std::filesystem::path(AdtTerrainMapsDirectory()) /
            Acore::StringFormat("{:03}{:02}{:02}.map", key >> 12, (key >> 6) & 63, key & 63), ignored);
        g_tiles.erase(oldest);
    }
    g_tiles[key] = Tile{ terrain, ++g_useClock };
    return terrain;
}

void LogAdtTerrainUse()
{
    uint32 made = g_made.exchange(0), missing = g_missing.exchange(0);
    uint64 bytes = g_adtBytes.exchange(0), us = g_makeUs.exchange(0);
    if (!made && !missing)
        return;
    std::size_t kept;
    {
        std::lock_guard<std::mutex> guard(g_tilesLock);
        kept = g_tiles.size();
    }
    LOG_INFO("module", "Terrain tiles made from the game's ADT files: {} ({} KB read), {} without terrain, {:.1f} ms each; {} kept",
        made, bytes / 1024, missing, (made + missing) ? us / 1000.0 / (made + missing) : 0.0, kept);
}
