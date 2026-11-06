/*
 * The static collision of the game's maps, made from the game's own files on first use, per grid: AzerothCore's vmap extractor (vmap/, read
 * through the running game) extracts the grid's ADT placements (doodads, buildings and the doodads of the buildings' sets) and the models
 * they name into raw files; AzerothCore's tile assembler converts each model (.vmo) and the server's model store loads it; the placements
 * become the server's own VMAP::ModelInstances. A map of a single building (its WDT places it, no ADTs) has that building in every grid.
 * All of it in the server's (memory) file system, under the temporary directory; the models are kept by the model store as with vmaps (their
 * converted files are deleted once loaded; the raw files stay: the extractor and the doodads' bounds read them again).
 * A grid's first making takes seconds (tens for the first grids of a session: the models), so it runs on a thread of its own: TryGet
 * gives the grid when it is made and asks for it to be made otherwise.
 */
#include "VmapTiles.h"
#include "AdtTerrain.h"
#include "Log.h"
#include "ModelInstance.h"
#include "TileAssembler.h"
#include "WorldModelStore.h"
#include "vmap/adtfile.h"
#include "vmap/vmapexport.h"
#include "vmap/wdtfile.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_set>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace
{
    // the models of a map: its instances by id (one instance for a model in several grids), its global building (a map of one building)
    struct MapModels
    {
        bool wdtRead = false;
        std::shared_ptr<VmapTile const> global;   // nullptr: the map has ADTs
        std::unordered_map<uint32, std::shared_ptr<VMAP::ModelInstance const>> byId;
    };

    std::mutex g_tilesLock;                         // the tiles
    std::unordered_map<uint64, int64> g_retryAt;     // grids whose make gave up waiting for the game: when they are tried again (under g_tilesLock)
    std::unordered_map<uint64, uint32> g_tries;
    int64 NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    std::unordered_map<uint64, std::shared_ptr<VmapTile const>> g_tiles;
    std::mutex g_makeLock;                          // one extraction at a time (the extractor's work data is global); the maps' models
    std::unordered_map<uint32, MapModels> g_maps;
    std::unordered_set<std::string> g_convertedModels;
    std::string g_rawDir, g_vmoDir;

    // the grids waiting to be made by the maker thread (under g_tilesLock)
    std::deque<std::pair<uint64, std::array<uint32, 3>>> g_queue;
    std::unordered_set<uint64> g_queued;
    std::condition_variable g_queueChanged;
    bool g_makerStarted = false;
    std::deque<uint64> g_evictQueue;   // grids to forget (under g_tilesLock): done by the maker thread, which owns the maps' model lists

    std::atomic<uint32> g_made{0}, g_empty{0}, g_converted{0}, g_evicted{0}, g_modelsReleased{0};
    std::atomic<uint64> g_gameBytes{0}, g_makeUs{0}, g_readUs{0}, g_instances{0};

    uint64 Key(uint32 mapId, uint32 gx, uint32 gy) { return uint64(mapId) << 12 | gx << 6 | gy; }

    // the work directories and the extractor's file reader (the first time); g_makeLock is held
    bool Prepare(WoWProcess* process)
    {
        if (!g_rawDir.empty())
            return true;
        std::filesystem::path base = std::filesystem::temp_directory_path() / "wowcontroller-vmaps";
        std::error_code error;
        std::filesystem::create_directories(base / "raw", error);
        std::filesystem::create_directories(base / "vmo", error);
        if (error)
        {
            LOG_ERROR("module", "{} could not be made: no static collision from the game", base.string());
            return false;
        }
        g_rawDir = (base / "raw").string();
        g_vmoDir = (base / "vmo").string();
        WoWVmap::szWorkDirWmo = g_rawDir.c_str();
        WoWVmap::SetGameFileReader([process](std::string const& name, std::vector<uint8>& contents)
        {
            auto const started = std::chrono::steady_clock::now();
            bool read = process->ReadFile(name, contents);
            g_readUs += uint64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
            if (read)
                g_gameBytes += contents.size();
            return read;
        });
        return true;
    }

    // the placements the extractor wrote (dir_bin: mapID, tileX, tileY, then a ModelSpawn each), as TileAssembler::readMapSpawns reads them
    std::vector<VMAP::ModelSpawn> ReadSpawns()
    {
        std::vector<VMAP::ModelSpawn> spawns;
        FILE* file = fopen((g_rawDir + "/dir_bin").c_str(), "rb");
        if (!file)
            return spawns;
        for (;;)
        {
            uint32 mapId, tileX, tileY;
            if (fread(&mapId, 4, 1, file) != 1 || fread(&tileX, 4, 1, file) != 1 || fread(&tileY, 4, 1, file) != 1)
                break;
            VMAP::ModelSpawn spawn;
            if (!VMAP::ModelSpawn::readFromFile(file, spawn))
                break;
            spawns.push_back(spawn);
        }
        fclose(file);
        return spawns;
    }

    // the instances of the placements (converting the models not converted yet), as TileAssembler::convertWorld2 and StaticMapTree do
    std::shared_ptr<VmapTile const> Instances(MapModels& models, std::vector<VMAP::ModelSpawn>& spawns)
    {
        VMAP::TileAssembler assembler(g_rawDir, g_vmoDir);
        auto tile = std::make_shared<VmapTile>();
        for (VMAP::ModelSpawn& spawn : spawns)
        {
            auto known = models.byId.find(spawn.ID);
            if (known != models.byId.end())
            {
                tile->instances.push_back(known->second);
                continue;
            }
            // (M2 placements have no bounds in the ADT; WMO maps and terrain maps use different origins: the assembler's adjustments)
            if (spawn.flags & VMAP::MOD_M2)
            {
                if (!assembler.calculateTransformedBound(spawn))
                    continue;
            }
            else if (spawn.flags & VMAP::MOD_WORLDSPAWN)
                spawn.iBound = spawn.iBound + G3D::Vector3(533.33333f * 32, 533.33333f * 32, 0.f);
            // (the store keeps a model once loaded: its converted file is not needed after that)
            bool convert = g_convertedModels.insert(spawn.name).second;
            if (convert)
            {
                if (!assembler.convertRawFile(spawn.name))
                    continue;
                ++g_converted;
            }
            std::shared_ptr<VMAP::WorldModel> model = sWorldModelStore->AcquireModelInstance(g_vmoDir + "/", spawn.name, spawn.flags);
            if (convert)
            {
                std::error_code ignored;
                std::filesystem::remove(g_vmoDir + "/" + spawn.name + ".vmo", ignored);
            }
            if (!model)
                continue;
            auto instance = std::make_shared<VMAP::ModelInstance const>(spawn, model);
            models.byId[spawn.ID] = instance;
            tile->instances.push_back(instance);
            ++g_instances;
        }
        return tile;
    }

    // extracts and converts grid (gx, gy) of the map; g_makeLock is held
    std::shared_ptr<VmapTile const> Make(WoWProcess* process, uint32 mapId, uint32 gx, uint32 gy)
    {
        if (!Prepare(process))
            return nullptr;
        std::string directory = GameMapDirectory(process, mapId);
        if (directory.empty())
            return nullptr;
        MapModels& models = g_maps[mapId];
        std::error_code ignored;
        std::string dirBin = g_rawDir + "/dir_bin";

        if (!models.wdtRead)
        {
            // the map's WDT: the building of a map of a single building (its spawn has tile 65, 65)
            models.wdtRead = true;
            std::filesystem::remove(dirBin, ignored);
            std::string wdtName = "World\\Maps\\" + directory + "\\" + directory + ".wdt";
            WoWVmap::WDTFile wdt(wdtName.data(), directory.data());
            if (wdt.init(mapId))
            {
                std::vector<VMAP::ModelSpawn> spawns = ReadSpawns();
                if (!spawns.empty())
                    models.global = Instances(models, spawns);
            }
        }
        if (models.global)
            return models.global;

        // (grid (gx, gy) of the server is the ADT <map>_<gy>_<gx>, as for the terrain; the extractor takes the file name's numbers)
        std::filesystem::remove(dirBin, ignored);
        std::string adtName = "World\\Maps\\" + directory + "\\" + directory + "_" + std::to_string(gy) + "_" + std::to_string(gx) + ".adt";
        WoWVmap::ADTFile adt(adtName.data());
        if (!adt.init(mapId, gy, gx))
            return nullptr;
        std::vector<VMAP::ModelSpawn> spawns = ReadSpawns();
        return Instances(models, spawns);
    }

    uint64 DirectoryBytes(std::string const& path)
    {
        uint64 bytes = 0;
        std::error_code error;
        for (auto const& entry : std::filesystem::directory_iterator(path, error))
            if (entry.is_regular_file(error))
                bytes += entry.file_size(error);
        return bytes;
    }

    // forgets the grids (the maker thread): their tiles go, the instances only they used, then the models only those instances used
    // (a converted model's file was deleted once loaded: it is converted again when it is needed again)
    void Forget(std::vector<uint64> const& keys)
    {
        {
            std::lock_guard<std::mutex> guard(g_tilesLock);
            for (uint64 key : keys)
                g_evicted += g_tiles.erase(key);
        }
        std::lock_guard<std::mutex> making(g_makeLock);
        for (auto& [mapId, models] : g_maps)
            for (auto instance = models.byId.begin(); instance != models.byId.end(); )
                instance = instance->second.use_count() == 1 ? models.byId.erase(instance) : std::next(instance);
        std::vector<std::string> names;
        g_modelsReleased += sWorldModelStore->ReleaseUnused(&names);
        for (std::string const& name : names)
            g_convertedModels.erase(name);
    }

    // makes the grid (if no other thread made it meanwhile) and keeps it
    std::shared_ptr<VmapTile const> MakeAndKeep(WoWProcess* process, uint64 key, uint32 mapId, uint32 gx, uint32 gy)
    {
        {
            std::lock_guard<std::mutex> guard(g_tilesLock);
            auto found = g_tiles.find(key);
            if (found != g_tiles.end())
                return found->second;
        }

        std::lock_guard<std::mutex> making(g_makeLock);
        {
            std::lock_guard<std::mutex> guard(g_tilesLock);   // (another thread may have made it meanwhile)
            auto found = g_tiles.find(key);
            if (found != g_tiles.end())
                return found->second;
        }
        auto const started = std::chrono::steady_clock::now();
        uint32 timeouts = process->Timeouts();
        std::shared_ptr<VmapTile const> tile = Make(process, mapId, gx, gy);
        g_makeUs += uint64(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        if (process->Timeouts() != timeouts)
        {
            // the game did not answer in time for a file of this grid: what was made may be incomplete, so it is not kept; it is made again later, with a growing delay
            std::lock_guard<std::mutex> guard(g_tilesLock);
            uint32& tries = g_tries[key];
            g_retryAt[key] = NowMs() + std::min<int64>(60000, int64(2000) << std::min<uint32>(tries, 5));
            ++tries;
            return nullptr;
        }
        ++(tile && !tile->instances.empty() ? g_made : g_empty);

        std::lock_guard<std::mutex> guard(g_tilesLock);
        g_tiles[key] = tile;
        g_tries.erase(key);
        g_retryAt.erase(key);
        return tile;
    }

    // the maker thread: the grids TryGet asked for, one after another
    void MakerThread(WoWProcess* process)
    {
        for (;;)
        {
            std::pair<uint64, std::array<uint32, 3>> next;
            std::vector<uint64> evict;
            {
                std::unique_lock<std::mutex> guard(g_tilesLock);
                g_queueChanged.wait(guard, [] { return !g_queue.empty() || !g_evictQueue.empty(); });
                evict.assign(g_evictQueue.begin(), g_evictQueue.end());
                g_evictQueue.clear();
                if (!g_queue.empty())
                {
                    next = g_queue.front();
                    g_queue.pop_front();
                }
                else
                    next.first = ~uint64(0);
            }
            if (!evict.empty())
                Forget(evict);
            if (next.first == ~uint64(0))
                continue;
            MakeAndKeep(process, next.first, next.second[0], next.second[1], next.second[2]);
            std::lock_guard<std::mutex> guard(g_tilesLock);
            g_queued.erase(next.first);
        }
    }
}

std::shared_ptr<VmapTile const> VmapTiles::TryGet(uint32 mapId, uint32 gx, uint32 gy, bool& ready)
{
    uint64 key = Key(mapId, gx, gy);
    std::lock_guard<std::mutex> guard(g_tilesLock);
    auto found = g_tiles.find(key);
    if (found != g_tiles.end())
    {
        ready = true;
        return found->second;
    }
    ready = false;
    auto retry = g_retryAt.find(key);
    if (retry != g_retryAt.end() && NowMs() < retry->second)
        return nullptr;   // (the game did not answer in time for this grid: not asked again yet)
    if (g_queued.insert(key).second)
    {
        g_queue.push_back({ key, { mapId, gx, gy } });
        if (!g_makerStarted)
        {
            g_makerStarted = true;
            std::thread(MakerThread, _process).detach();
        }
        g_queueChanged.notify_one();
    }
    return nullptr;
}

void VmapTiles::Evict(uint32 mapId, uint32 gx, uint32 gy)
{
    std::lock_guard<std::mutex> guard(g_tilesLock);
    g_evictQueue.push_back(Key(mapId, gx, gy));
    if (!g_makerStarted)
    {
        g_makerStarted = true;
        std::thread(MakerThread, _process).detach();
    }
    g_queueChanged.notify_one();
}

std::shared_ptr<VmapTile const> VmapTiles::Get(uint32 mapId, uint32 gx, uint32 gy)
{
    return MakeAndKeep(_process, Key(mapId, gx, gy), mapId, gx, gy);
}

std::string DescribeVmapTilesUse(bool reset)
{
    uint32 made = reset ? g_made.exchange(0) : g_made.load(), empty = reset ? g_empty.exchange(0) : g_empty.load();
    uint32 converted = reset ? g_converted.exchange(0) : g_converted.load();
    uint32 evicted = reset ? g_evicted.exchange(0) : g_evicted.load(), released = reset ? g_modelsReleased.exchange(0) : g_modelsReleased.load();
    uint64 bytes = reset ? g_gameBytes.exchange(0) : g_gameBytes.load(), us = reset ? g_makeUs.exchange(0) : g_makeUs.load();
    uint64 readUs = reset ? g_readUs.exchange(0) : g_readUs.load();
    if (!made && !empty && !evicted)
        return std::string();
    std::size_t kept;
    {
        std::lock_guard<std::mutex> guard(g_tilesLock);
        kept = g_tiles.size();
    }
    uint64 rawBytes = 0, vmoBytes = 0;
    {
        std::lock_guard<std::mutex> making(g_makeLock);
        if (!g_rawDir.empty())
        {
            rawBytes = DirectoryBytes(g_rawDir);
            vmoBytes = DirectoryBytes(g_vmoDir);
        }
    }
    std::ostringstream text;
    text << "static collision tiles made from the game's files: " << made << " (" << empty << " without models), " << converted
         << " models converted, " << bytes / 1024 << " KB read from the game in " << readUs / 1000 << " ms, "
         << (made + empty ? us / 1000 / (made + empty) : 0) << " ms each in all; " << kept << " kept, " << g_instances.load() << " model instances, work files " << rawBytes / 1024 << " KB raw + "
         << vmoBytes / 1024 << " KB converted; " << evicted << " grids forgotten, " << released << " models released";
    return text.str();
}
