/*
 * The game in the page around the server (the WebAssembly build of the server embedded in the game's web page): requests go to the page
 * (Module.wowController.request, which the page passes when it creates the server module) and the calling thread waits on the request's
 * state word until the page has the answer from the game. The answer is written into memory the page allocates with malloc; it is freed here.
 */
#include "WoWProcess.h"

#ifdef __EMSCRIPTEN__
#include "Log.h"
#include "World.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <emscripten.h>
#include <emscripten/threading.h>

namespace
{
    // The request is a block of this module's memory that the page reads and writes while the calling thread waits. Every request has a time limit, and
    // the block is owned in a way that makes the limit safe: whoever comes second frees it. When the caller gives up (the state word goes from 0 to -2) the
    // page frees the block, and the answer it may still bring, when it finally answers; when the page answered first, the caller frees both.
    struct Request   // the layout the page reads (web/acore.js, Acore_ControllerRequest)
    {
        volatile int32 state;   // 0 waiting, 1 answered, -1 no answer, -2 the caller gave up
        int32 kind;             // 1 a file, 2 a command, 3 world queries, 5 keep the files now
        int32 text;             // the name or the command line (UTF-8, NUL terminated): in the block, after this header
        int32 data;             // the answer (malloc); kind 3: first the queries, in the block
        int32 size;
    };

    constexpr double FileLimitMs = 2000, CommandLimitMs = 10000, QueryLimitMs = 1500;
    constexpr double Patient = -1;   // (no limit: until the page answers or the server is told to stop)
    constexpr double FirstReportSeconds = 30;   // a patient wait writes a line after this long, then after twice as long, and so on

    class PageProcess : public WoWProcess
    {
    public:
        std::string Describe() const override { return "the game in this page"; }

        bool ReadFile(std::string const& name, std::vector<uint8>& contents, bool patient) override
        {
            Answer a;
            if (!Ask(1, name, nullptr, 0, patient ? Patient : FileLimitMs, a))
                return false;
            contents.assign(reinterpret_cast<uint8*>(a.data), reinterpret_cast<uint8*>(a.data) + a.size);
            free(reinterpret_cast<void*>(a.data));
            return true;
        }

        std::optional<std::string> Command(std::string const& line) override
        {
            Answer a;
            if (!Ask(2, line, nullptr, 0, CommandLimitMs, a))
                return std::nullopt;
            std::string answer(reinterpret_cast<char*>(a.data), a.size);
            free(reinterpret_cast<void*>(a.data));
            return answer;
        }

        bool HasWorld() const override { return true; }

        bool WorldQueries(WoWWorldQuery* queries, std::size_t count) override
        {
            Answer a;
            std::size_t bytes = count * sizeof(WoWWorldQuery);
            if (!Ask(3, {}, queries, bytes, QueryLimitMs, a))
                return false;
            if (static_cast<std::size_t>(a.size) == bytes)
                memcpy(queries, reinterpret_cast<void*>(a.data), bytes);
            free(reinterpret_cast<void*>(a.data));
            return true;
        }

        // the page writes the server's kept files (the characters database among them) to the browser's storage now; waits for the page to take the request
        void KeepNow() override
        {
            Answer a;
            if (Ask(5, {}, nullptr, 0, QueryLimitMs, a))
                free(reinterpret_cast<void*>(a.data));
        }

        bool LastTimedOut() const override { return _timedOut; }
        uint32 Timeouts() const override { return _timeouts; }

    private:
        struct Answer { int32 data = 0, size = 0; };

        bool Ask(int32 kind, std::string const& text, void const* input, std::size_t inputSize, double limitMs, Answer& out)
        {
            _timedOut = false;
            if (emscripten_is_main_browser_thread())
                return false;   // (the page answers on the main thread: it cannot be waited for there)
            std::size_t total = sizeof(Request) + text.size() + 1 + inputSize;
            Request* r = static_cast<Request*>(malloc(total));
            if (!r)
                return false;
            memset(r, 0, sizeof(Request));
            char* t = reinterpret_cast<char*>(r + 1);
            memcpy(t, text.c_str(), text.size() + 1);
            r->text = static_cast<int32>(reinterpret_cast<uintptr_t>(t));
            if (inputSize)
            {
                char* d = t + text.size() + 1;
                memcpy(d, input, inputSize);
                r->data = static_cast<int32>(reinterpret_cast<uintptr_t>(d));
                r->size = static_cast<int32>(inputSize);
            }
            int32 given = r->data;   // (kind 3: the queries; the answer is a new block)
            r->kind = kind;
            r->state = 0;
            MAIN_THREAD_ASYNC_EM_ASM({ Module['wowController'].request($0, function () { return wasmMemory.buffer; }); }, r);
            bool patient = limitMs < 0;
            double waited = 0;
            auto start = std::chrono::steady_clock::now();
            double nextReport = FirstReportSeconds;
            bool stopped = false;
            while (__atomic_load_n(&r->state, __ATOMIC_SEQ_CST) == 0 && (patient || waited < limitMs))
            {
                double step = patient ? 100.0 : std::min(100.0, limitMs - waited);
                emscripten_futex_wait(const_cast<int32*>(&r->state), 0, step);
                waited += step;
                if (!patient)
                    continue;
                if (World::IsStopped())
                {
                    stopped = true;   // (the server is told to stop: the start-up ends the way a stopped server ends)
                    break;
                }
                double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (seconds >= nextReport)
                {
                    LOG_INFO("module", "Still waiting for the game to give {} ({} s)", text, static_cast<int>(seconds));
                    nextReport *= 2;
                }
            }
            int32 expected = 0;
            if (__atomic_compare_exchange_n(&r->state, &expected, -2, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            {
                if (!stopped)   // (a patient request that was stopped is not a timeout: it never had a limit)
                {
                    _timedOut = true;   // (the page frees the block and any answer it brings later)
                    ++_timeouts;
                }
                return false;
            }
            bool answered = r->state > 0 && r->data != 0 && r->data != given;
            out.data = r->data;
            out.size = r->size;
            free(r);
            return answered;
        }

        thread_local static bool _timedOut;
        thread_local static uint32 _timeouts;
    };

    thread_local bool PageProcess::_timedOut = false;
    thread_local uint32 PageProcess::_timeouts = 0;
}

std::unique_ptr<WoWProcess> CreatePageProcess()
{
    if (!MAIN_THREAD_EM_ASM_INT({ return (Module['wowController'] && Module['wowController'].request) ? 1 : 0; }))
        return nullptr;
    return std::make_unique<PageProcess>();
}

#else

std::unique_ptr<WoWProcess> CreatePageProcess()
{
    return nullptr;
}

#endif
