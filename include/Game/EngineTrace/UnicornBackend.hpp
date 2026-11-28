#pragma once

#include "Game/EngineTrace.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct uc_struct;

namespace OW::EngineTrace {

// Page bytes shared by every engine in the process. One DMA read serves all engines;
// each engine still copies the page into its own Unicorn mapping.
class PageStore {
public:
    struct Stats {
        uint64_t fetches = 0;  // pages read from the target
        uint64_t hits = 0;     // pages served from this store
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<uint64_t, std::vector<uint8_t>> pages;
    std::unordered_set<uint64_t> inFlightPages;
    Stats stats;
};

// Configuration for the UniCore backend. Everything is explicit: no defaults that silently
// point at a different build.
struct UnicornConfig {
    std::string processName = "Overwatch.exe";
    // World 155284 wrapper. CN 155284N uses 0x10222E0.
    uint64_t entryRva = 0xD0E030;
    // Optional 0x240-byte setup capture; when empty a synthetic setup is built below.
    std::string setupTemplatePath;
    uint64_t mask = 0x0690000000000017ull;
    uint64_t context = 0;
    uint64_t context2 = 0;
    // setup + 0x1E8: the level/world object the core dereferences before it runs the
    // query. Every live 0x240 capture carries a non-zero heap pointer here; a zero makes
    // the core jump straight to its epilogue without writing the result (the 2026-11-21
    // "synthetic setup never reached the write branch" failure).
    uint64_t filterObject = 0;
    // setup + 0x1A0: world-trace callback pointer (build constant, profile RVA).
    uint64_t auxCallback = 0;
    // setup + 0x1F8: second level object read by the core before the query.
    uint64_t sceneObject2 = 0;
    // setup + 0x68 count / + 0x70 items: the game's ray filter entries (self/team
    // exclusions). Empty lists make the world query hit the local player's own body.
    uint32_t filterCount = 0;
    uint64_t filterItems[4] = {};
    // Optional per-trace filter item source. The first list entry is refreshed from the
    // live local entity so a hero swap cannot leave a stale parent pointer behind.
    std::function<uint64_t()> filterItemResolver;
    // setup + 0x40: per-call local ray object read by the core. Live setups carry a
    // non-zero entity object here.
    uint64_t localObject = 0;
    uint64_t flags = 0;
    uint64_t instructionLimit = 4'000'000;
    // 0 means "no timer". A non-zero Unicorn timeout installs a 15 ms host timer whose
    // only useful effect here is a fixed latency floor; instructionLimit already bounds
    // a runaway trace, and the module-fetch hook stops execution outside the image.
    uint64_t timeoutUs = 0;
    // Paged-out .text policy: a failed page read is never cached and the next request
    // retries it. A short in-call retry absorbs the page-in race without hiding a real
    // failure; when the retries are exhausted the request returns Error and the broker's
    // asynchronous caller simply asks again on its next tick.
    uint32_t pageReadRetries = 2;
    uint32_t pageReadRetryDelayUs = 500;
    // Re-read the pages touched by the previous trace before each new one. Off means the
    // scene snapshot is reused (the "warm" case this backend exists to measure).
    bool refreshTouchedPages = false;
    // Skip the per-instruction hook. The native instruction count still bounds the run;
    // a fetch outside the module is still stopped by the unmapped-fetch hook.
    bool skipCodeHook = false;
};

// UniCore backend: one persistent Unicorn engine, pages mapped once and kept, world pages
// cached between traces. It answers the ray contract in Game/EngineTrace.hpp.
class UnicornBackend final : public IBackend {
public:
    explicit UnicornBackend(UnicornConfig config);
    ~UnicornBackend() override;

    UnicornBackend(const UnicornBackend&) = delete;
    UnicornBackend& operator=(const UnicornBackend&) = delete;

    const char* Name() const override { return "unicorn"; }
    bool BindScene(uint64_t sceneEpoch) override;
    Status Trace(const Request& request, Result& result) override;
    void ReleaseScene() override;

    struct Stats {
        uint64_t traces = 0;
        uint64_t pageFetches = 0;   // DMA reads performed for pages
        uint64_t pageReadFailures = 0;
        uint64_t pageReadRetries = 0;       // retries performed after a failed page read
        uint64_t pageUnavailableTraces = 0; // traces aborted by an unreadable page
        uint64_t lastFailedPage = 0;
        uint64_t cacheHits = 0;     // page already present in the host cache
        uint64_t mappedPages = 0;   // new uc_mem_map calls
        uint64_t faults = 0;        // emulated page faults
        uint64_t instructions = 0;  // last trace
        uint64_t lastUs = 0;
        uint64_t setupUs = 0;
        uint64_t emuUs = 0;
        uint64_t prepUs = 0;
        uint64_t maxUs = 0;
    };
    const Stats& GetStats() const { return stats_; }
    void ResetStats() { stats_ = Stats{}; }

    // Product integration seam: when set, page bytes come from the host reader (for example
    // Memory::Read) instead of this backend opening its own DMA handle.
    using Reader = std::function<bool(uint64_t, void*, size_t)>;
    void SetReader(Reader reader) { reader_ = std::move(reader); }
    // Target identity for the reader path: module base, image size and (optionally) a TEB.
    void SetTarget(uint64_t base, uint32_t imageSize, uint64_t teb = 0) {
        base_ = base;
        imageSize_ = imageSize;
        teb_ = teb;
        externalTarget_ = true;
    }
    void SetPageStore(std::shared_ptr<PageStore> store) { sharedStore_ = std::move(store); }
    uint64_t Base() const { return base_; }
    uint64_t Context() const { return context_; }
    bool ProcessOpen() const { return vmm_ != nullptr; }
    bool ContextAlive(uint64_t context) const;

    // Hook entry points. The Unicorn callbacks in the translation unit forward here so the
    // header stays independent of the Unicorn ABI.
    bool OnFault(int type, uint64_t address);
    bool OnInstruction(uint64_t address);
    void OnWrite(uint64_t address, int size);

private:
    bool OpenProcess();
    void CloseProcess();
    void LoadThreads();
    bool EnsurePage(uint64_t page);
    bool MapSynthetic(uint64_t page);
    bool InImage(uint64_t address) const;
    bool ReadTarget(uint64_t address, void* out, size_t size);
    bool ReadPage(uint64_t page, uint8_t* out);
    bool FetchPageWithRetry(uint64_t page, uint8_t* out);
    void ResetEngineRuntime();
    bool PrepareSetup();
    bool RefreshTouchedPages();

    UnicornConfig config_;
    void* vmm_ = nullptr;
    uint32_t pid_ = 0;
    uint64_t base_ = 0;
    uint32_t imageSize_ = 0;
    uint64_t teb_ = 0;

    uc_struct* uc_ = nullptr;
    Reader reader_;
    bool externalTarget_ = false;
    std::shared_ptr<PageStore> sharedStore_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> pageBytes_;
    std::unordered_set<uint64_t> mapped_;
    std::unordered_set<uint64_t> touched_;
    std::vector<uint8_t> setupTemplate_;
    uint64_t context_ = 0;
    bool bound_ = false;
    uint64_t sceneEpoch_ = 0;

    bool stop_ = false;
    uint8_t fractionWritten_ = 0;
    uint64_t instructionCount_ = 0;
    bool pageFailure_ = false;  // this trace tried to read a page and failed
    Stats stats_{};
};

}  // namespace OW::EngineTrace






