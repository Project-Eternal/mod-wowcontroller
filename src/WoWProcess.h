/*
 * A World of Warcraft client that the server can use (mod-wowcontroller): it reads the game's files the way the game does and takes commands.
 * The module behaves as if it ran a little headless game next to the server; where the server is embedded in the game's page, the "process"
 * is the game that is already running there (PageProcess). Elsewhere it can be an extracted game in a directory (DirectoryProcess).
 */
#ifndef MOD_WOWCONTROLLER_WOWPROCESS_H
#define MOD_WOWCONTROLLER_WOWPROCESS_H

#include "Define.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

// a question about the game's world and its answer (the layout of the port's WoW-world.h: the page copies these records between the
// server and the game as they are; little endian, 4-byte fields)
struct WoWWorldQuery
{
    enum Op : uint32
    {
        State          = 0,   // a0 the game's map, a1 the loaded terrain tiles (always known)
        TerrainHeight  = 1,   // p0 p1 (x, y): a0 hit, f0 the terrain's height (flags: the trace's, 0 = the terrain)
        TerrainArea    = 2,   // p0 p1 p2: a0 the area (AreaTable.dbc) of the terrain
        Liquid         = 3,   // p0 p1 p2: a0 found, a1 the liquid type (LiquidType.dbc), f0 its level
        Trace          = 4,   // p0..p2 to p3..p5 with the flags: a0 hit, f0..f2 the hit point
        Tiles          = 5,   // p0 a row of terrain tiles (tileX 0..63): a0, a1 the loaded ones (bit tileY), a2 the game's map (always known; not used by the module)
        Building       = 6,   // p0 p1 p2: the building (WMO) group below: a0 found, f0 its floor z, a1 root id, a2 name set (adt id); the raw bits of
                              // f1 the group id, of f2 the group's flags (MOGP)
        BuildingLiquid = 7    // p0 p1 p2: the liquid of a building group there: a0 found, a1 the liquid type, f0 its level
    };

    // the game's trace flags (what a trace hits)
    static constexpr uint32 TraceDoodads = 0x1;    // M2 models
    static constexpr uint32 TraceBuildings = 0x10; // WMO models
    static constexpr uint32 TraceTerrain = 0x100;
    static constexpr uint32 TraceGameObjects = 0x100000;   // the game objects' models, as they are now (an open door is not hit): doodad models
                                                           // (doors) and buildings (destructible buildings)
    // (the port's own bits, not the game's: which of the server's two sets of models a trace or a building question is about; the game's
    // building flag 0x10 hits the buildings of both: the game passes through the hits of the other set; WoW-world.h WOW_SET_*)
    static constexpr uint32 SetMap = 0x80000000;
    static constexpr uint32 SetGameObjects = 0x08000000;

    uint32 op = State, map = 0, flags = 0;
    float p[6] = {};
    // the answer: known = 0 when the game cannot tell (another map, terrain not loaded): the server's "no data"
    int32 known = 0;
    uint32 a[3] = {};
    float f[3] = {};
};
static_assert(sizeof(WoWWorldQuery) == 64, "the layout of WoW-world.h");

class WoWProcess
{
public:
    virtual ~WoWProcess() = default;

    // a line for the log: what this process is
    virtual std::string Describe() const = 0;

    // a file of the game by its name in the game ("DBFilesClient\\Spell.dbc"): the whole file, or false when the game does not have it.
    // patient: for the files the server reads while it starts (nothing else depends on the wait, and the game is the only source): wait for the
    // answer as long as it takes (the log says so every 30 s, 60 s, 120 s...) and give up only when the server is told to stop; a definite "no" is
    // still an immediate false. Processes that answer at once ignore it.
    virtual bool ReadFile(std::string const& name, std::vector<uint8>& contents, bool patient = false) = 0;

    // a command for the game (the port's chat commands: "gfx flat", "log perf", "prof"): its answer, or nothing when the game does not know it
    // or cannot take commands
    virtual std::optional<std::string> Command(std::string const& line) = 0;

    // whether the last ReadFile, Command or WorldQueries of this thread gave up waiting (no answer within its limit) rather than being told "no"
    virtual bool LastTimedOut() const { return false; }

    // how many requests of this thread have given up waiting so far (a piece of work that was made while it grew may be incomplete: it is made again later)
    virtual uint32 Timeouts() const { return 0; }

    // asks the page that holds the server to write the server's kept files to the browser's storage now (it does not wait for the write itself)
    virtual void KeepNow() { }

    // whether the game answers questions about its world (WorldQueries)
    virtual bool HasWorld() const { return false; }

    // the answers to questions about the game's world (in place); false when the game could not be asked (every answer unknown)
    virtual bool WorldQueries(WoWWorldQuery* queries, std::size_t count) { (void) queries; (void) count; return false; }
};

// the process chosen by the configuration (WoWController.Process), created at the first use; nullptr when there is none
WoWProcess* GetWoWProcess();

std::unique_ptr<WoWProcess> CreatePageProcess();                                   // nullptr when the server does not run in a page with a game
std::unique_ptr<WoWProcess> CreateDirectoryProcess(std::string const& directory);  // nullptr when the directory does not exist

#endif
