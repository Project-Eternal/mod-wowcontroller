/*
 * mod-wowcontroller: the server uses a World of Warcraft client next to it (WoWProcess.h).
 *   - the data files are read from the game as the game itself sees them (DataFileSource, the core's hook): the DBC stores (DataDir/dbc/X.dbc =
 *     DBFilesClient\X.dbc) and the cinematic cameras (DataDir/Cameras/X.m2 = Cameras\X.m2); files the game does not give are read from disk
 *   - the world as the game sees it (MapDataProvider, the core's hook; WoWMapData.cpp): terrain height, area, liquid, line of sight and hits
 *     with the models are asked of the game instead of maps/ and vmaps/ (WoWController.World)
 *   - .wow COMMAND: a command for the game (the port's own: .wow gfx flat, .wow log perf, .wow prof); .wow file NAME: what the game has under NAME
 * Configuration: conf/mod_wowcontroller.conf.dist.
 */
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DataFileSource.h"
#include "GridDefines.h"
#include "AdtTerrain.h"
#include "Log.h"
#include "Map.h"
#include "MapDataProvider.h"
#include "NavTiles.h"
#include "ModelIgnoreFlags.h"
#include "ModelInstance.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Transport.h"
#include "VMapMgr2.h"
#include "VmapTiles.h"
#include "World.h"
#include "WorldSession.h"
#include "WoWMapData.h"
#include "WoWProcess.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>

using namespace Acore::ChatCommands;

namespace
{
    std::once_flag g_processOnce;
    std::unique_ptr<WoWProcess> g_process;
    bool g_dbc = true, g_cameras = true;

    // what was read, per kind of data file
    struct FileCounts
    {
        std::atomic<uint32> fromGame{0}, fromDisk{0};
        std::atomic<uint64> bytes{0};
    } g_dbcCounts, g_cameraCounts;

    // the game's name of a data file the server opens: "DATADIR/dbc/Spell.dbc" -> "DBFilesClient\Spell.dbc", "DATADIR/Cameras/FlyByHuman.m2" ->
    // "Cameras\FlyByHuman.m2"; empty for what is not the game's (the locale folders dbc/deDE/..., files outside DataDir) or switched off
    std::string GameNameOf(std::string const& path, FileCounts*& counts)
    {
        std::string dataPath = sWorld->GetDataPath(), relative = path;
        std::replace(dataPath.begin(), dataPath.end(), '\\', '/');
        std::replace(relative.begin(), relative.end(), '\\', '/');
        if (!dataPath.empty() && relative.compare(0, dataPath.size(), dataPath) == 0)
            relative = relative.substr(dataPath.size());
        else if (std::size_t at = relative.rfind("/dbc/"); at != std::string::npos)
            relative = relative.substr(at + 1);   // (a DataDir written another way: ./data/ against data/)
        else
            return {};

        if (relative.compare(0, 4, "dbc/") == 0)
        {
            std::string file = relative.substr(4);
            if (!g_dbc || file.empty() || file.find('/') != std::string::npos)
                return {};
            counts = &g_dbcCounts;
            return "DBFilesClient\\" + file;
        }
        if (relative.compare(0, 8, "Cameras/") == 0 && g_cameras)
        {
            counts = &g_cameraCounts;
            std::replace(relative.begin(), relative.end(), '/', '\\');
            return relative;
        }
        return {};
    }

    bool ReadFromGame(std::string const& path, std::vector<uint8>& contents)
    {
        FileCounts* counts = nullptr;
        std::string name = GameNameOf(path, counts);
        if (name.empty())
            return false;   // (AzerothCore probes dbc/<locale>/ for every locale: the game has only its own)
        WoWProcess* process = GetWoWProcess();
        // (the server's start-up reads: the game is the only source of a deployment that has no files of its own, so wait for it)
        if (process && process->ReadFile(name, contents, true))
        {
            ++counts->fromGame;
            counts->bytes += contents.size();
            if (name == "DBFilesClient\\Map.dbc")
                NoteGameMapDbc(contents);   // (the terrain converter needs its folder names: no second, impatient read)
            return true;
        }
        ++counts->fromDisk;
        if (process && !World::IsStopped())
            LOG_INFO("module", "{} is not given by the game: read from disk", path);
        return false;
    }
}

WoWProcess* GetWoWProcess()
{
    std::call_once(g_processOnce, []()
    {
        if (!sConfigMgr->GetOption<bool>("WoWController.Enable", true))
            return;
        std::string kind = sConfigMgr->GetOption<std::string>("WoWController.Process", "auto");
        std::string directory = sConfigMgr->GetOption<std::string>("WoWController.Directory", "");
        if (kind == "page" || kind == "auto")
            g_process = CreatePageProcess();
        if (!g_process && (kind == "directory" || kind == "auto"))
            g_process = CreateDirectoryProcess(directory);
        if (g_process)
            LOG_INFO("module", "Using {}", g_process->Describe());
        else if (kind != "auto" || !directory.empty())
            LOG_ERROR("module", "No game to use (WoWController.Process = \"{}\", WoWController.Directory = \"{}\")", kind, directory);
    });
    return g_process.get();
}

class WoWControllerWorld : public WorldScript
{
public:
    WoWControllerWorld() : WorldScript("WoWControllerWorld", { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    void OnUpdate(uint32 diff) override
    {
        // how much the world questions cost, once a minute
        _sinceLog += diff;
        if (_sinceLog < 60000)
            return;
        LogWoWMapDataUse(_sinceLog / 1000);
        _sinceLog = 0;
    }

    void OnStartup() override
    {
        for (auto const& [kind, counts] : { std::pair<char const*, FileCounts const*>{ "DBC files", &g_dbcCounts },
                                            std::pair<char const*, FileCounts const*>{ "camera files", &g_cameraCounts } })
            if (counts->fromGame || counts->fromDisk)
                LOG_INFO("module", "{}: {} from the game ({} KB), {} from DataDir", kind, counts->fromGame.load(),
                    counts->bytes.load() / 1024, counts->fromDisk.load());
    }

private:
    uint32 _sinceLog = 0;
};

// where the players are, for the world questions (WoWMapData.cpp)
class WoWControllerPlayers : public PlayerScript
{
public:
    WoWControllerPlayers() : PlayerScript("WoWControllerPlayers", { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_BEFORE_TELEPORT }) { }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        // done loading (login, a far teleport) or a teleport on the map: what the server worked out for the position meanwhile had no map
        // data (zone, area, floor, liquid): again now
        if (NoteWoWMapDataPlayer(player, false))
            player->UpdatePositionData();
    }
    void OnPlayerLogin(Player* player) override
    {
        if (NoteWoWMapDataPlayer(player, false))
            player->UpdatePositionData();
    }
    void OnPlayerLogout(Player* player) override { NoteWoWMapDataPlayer(player, true); }
    void OnPlayerMapChanged(Player* player) override
    {
        if (NoteWoWMapDataPlayer(player, false))
            player->UpdatePositionData();
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float /*x*/, float /*y*/, float /*z*/, float /*orientation*/, uint32 /*options*/,
        Unit* /*target*/) override
    {
        NoteWoWMapDataTeleport(player, mapId);
        return true;
    }
};

// the doors for the navigation meshes: a closed door is an obstacle in them, an open one is not (NavTiles); paths near doors and
// destructible buildings are checked with the game (WoWMapData CheckPath)
class WoWControllerDoors : public AllGameObjectScript
{
public:
    WoWControllerDoors() : AllGameObjectScript("WoWControllerDoors") { }

    void OnGameObjectAddWorld(GameObject* go) override { Changed(go, false); }
    void OnGameObjectStateChanged(GameObject* go, uint32 /*state*/) override { Changed(go, false); }
    void OnGameObjectRemoveWorld(GameObject* go) override { Changed(go, true); }

private:
    static void Changed(GameObject const* go, bool removed)
    {
        NavTiles::DoorChanged(go, removed);
        NoteWoWMapDataGameObject(go, removed);
    }
};

class WoWControllerCommands : public CommandScript
{
public:
    WoWControllerCommands() : CommandScript("WoWControllerCommands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wowTable =
        {
            { "file",      HandleFileCommand,      SEC_GAMEMASTER, Console::Yes },
            { "transport", HandleTransportCommand, SEC_GAMEMASTER, Console::No },
            { "vmaps",     HandleVmapsCommand,     SEC_GAMEMASTER, Console::No },
            { "",          HandleWoWCommand,       SEC_GAMEMASTER, Console::Yes }
        };
        static ChatCommandTable commandTable =
        {
            { "wow", wowTable }
        };
        return commandTable;
    }

    // .wow file NAME: whether the game has the file and its size
    static bool HandleFileCommand(ChatHandler* handler, Tail name)
    {
        WoWProcess* process = GetWoWProcess();
        if (!process)
        {
            handler->SendSysMessage("No game is connected (mod-wowcontroller).");
            return true;
        }
        std::vector<uint8> contents;
        if (process->ReadFile(std::string(name), contents))
            handler->PSendSysMessage("{}: {} bytes", std::string(name), contents.size());
        else
            handler->PSendSysMessage("{}: the game does not have it", std::string(name));
        return true;
    }

    // .wow transport: the transport the server finds under the player (Map::GetTransportForPos, as for a pet of a player there or an object a
    // spell places there); .wow transport SPAWNID: whether the map data provider's answer for that game object's model
    // (MapDataProvider::IntersectGameObject, which stands in for the model from vmaps) hits it: a ray down through where the server has it now
    static bool HandleTransportCommand(ChatHandler* handler, Optional<uint32> spawnId)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            return false;
        if (!spawnId)
        {
            Transport* transport = player->GetMap()->GetTransportForPos(player->GetPhaseMask(), player->GetPositionX(), player->GetPositionY(),
                player->GetPositionZ(), player);
            if (transport)
                handler->PSendSysMessage("Transport under the player: {} (guid {}, entry {}) at {} {} {}", transport->GetName(), transport->GetSpawnId(),
                    transport->GetEntry(), transport->GetPositionX(), transport->GetPositionY(), transport->GetPositionZ());
            else
                handler->SendSysMessage("No transport under the player");
            return true;
        }
        MapDataProvider* provider = MapDataProvider::Get();
        if (!provider)
        {
            handler->SendSysMessage("No map data provider (WoWController.World off)");
            return true;
        }
        auto spawns = player->GetMap()->GetGameObjectBySpawnIdStore().equal_range(*spawnId);
        if (spawns.first == spawns.second)
            handler->PSendSysMessage("No game object {} on the player's map", *spawnId);
        for (auto spawn = spawns.first; spawn != spawns.second; ++spawn)
        {
            GameObject* go = spawn->second;
            float startZ = go->GetPositionZ() + 15.0f, dist = 30.0f;
            bool hit = provider->IntersectGameObject(*go, go->GetPositionX(), go->GetPositionY(), startZ, 0.0f, 0.0f, -1.0f, dist);
            if (hit)
                handler->PSendSysMessage("{} (guid {}) at {} {} {}: hit at z {}", go->GetName(), go->GetSpawnId(), go->GetPositionX(), go->GetPositionY(),
                    go->GetPositionZ(), startZ - dist);
            else
                handler->PSendSysMessage("{} (guid {}) at {} {} {}: no hit (or unknown)", go->GetName(), go->GetSpawnId(), go->GetPositionX(),
                    go->GetPositionY(), go->GetPositionZ());
        }
        return true;
    }

    // .wow vmaps: the static collision of the player's grid made from the game's files (VmapTiles: the first time it is made), what that took,
    // and the floor under the player found in it (as the server's vmap height: a ray down from 2 yd above, 10 yd)
    static bool HandleVmapsCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        WoWProcess* process = GetWoWProcess();
        if (!player || !player->IsInWorld() || !process)
            return false;
        static VmapTiles tiles(process);
        GridCoord grid = Acore::ComputeGridCoord(player->GetPositionX(), player->GetPositionY());
        auto const started = std::chrono::steady_clock::now();
        std::shared_ptr<VmapTile const> tile = tiles.Get(player->GetMapId(), grid.x_coord, grid.y_coord);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (!tile)
        {
            handler->PSendSysMessage("Grid {} {} of map {}: no static collision from the game ({:.0f} ms)", grid.x_coord, grid.y_coord,
                player->GetMapId(), ms);
            return true;
        }
        G3D::Vector3 from = VMAP::VMapMgr2::convertPositionToInternalRep(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ() + 2.0f);
        G3D::Ray ray(from, G3D::Vector3(0, 0, -1));
        float nearest = 12.0f;
        bool hit = false;
        for (auto const& instance : tile->instances)
        {
            float distance = nearest;
            if (instance->intersectRay(ray, distance, false, VMAP::ModelIgnoreFlags::Nothing) && distance < nearest)
            {
                nearest = distance;
                hit = true;
            }
        }
        handler->PSendSysMessage("Grid {} {} of map {}: {} models placed, {:.0f} ms; floor under the player: {}", grid.x_coord, grid.y_coord,
            player->GetMapId(), tile->instances.size(), ms, hit ? std::to_string(from.z - nearest) : std::string("none within 10 yd"));
        std::string use = DescribeVmapTilesUse(false);
        if (!use.empty())
            handler->SendSysMessage(use);
        return true;
    }

    // .wow COMMAND...: the command goes to the game, its answer lines come back
    static bool HandleWoWCommand(ChatHandler* handler, Tail line)
    {
        WoWProcess* process = GetWoWProcess();
        if (!process)
        {
            handler->SendSysMessage("No game is connected (mod-wowcontroller).");
            return true;
        }
        std::optional<std::string> answer = process->Command(std::string(line));
        if (!answer)
        {
            handler->PSendSysMessage("The game does not know \"{}\".", std::string(line));
            return true;
        }
        std::istringstream lines(*answer);
        for (std::string text; std::getline(lines, text); )
            handler->SendSysMessage(text);
        // .wow prof: the server's side of the world questions too
        if (std::string(line).compare(0, 4, "prof") == 0)
            for (std::string const& text : DescribeWoWMapDataUse())
                handler->SendSysMessage(text);
        return true;
    }
};

void AddWoWControllerScripts()
{
    g_dbc = sConfigMgr->GetOption<bool>("WoWController.Dbc", true);
    g_cameras = sConfigMgr->GetOption<bool>("WoWController.Cameras", true);
    if (sConfigMgr->GetOption<bool>("WoWController.Enable", true) && (g_dbc || g_cameras))
        DataFileSource::Set(ReadFromGame);
    if (sConfigMgr->GetOption<bool>("WoWController.Enable", true) && sConfigMgr->GetOption<bool>("WoWController.World", true))
    {
        WoWProcess* process = GetWoWProcess();
        if (process && process->HasWorld())
        {
            bool paths = sConfigMgr->GetOption<bool>("WoWController.Paths", true);
            MapDataProvider::Set(CreateWoWMapData(process, paths));
            if (paths)
                new WoWControllerDoors();
            new WoWControllerPlayers();
            LOG_INFO("module", "The world's terrain and collision near the player are asked of the game (maps/ and vmaps/ are not read)");
        }
    }
    new WoWControllerWorld();
    new WoWControllerCommands();
}
