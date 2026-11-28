#include "Game/EngineTrace/UnicornBackend.hpp"

#include <windows.h>

#include <unicorn/unicorn.h>
#include <vmmdll.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

namespace OW::EngineTrace {
namespace {

constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kStackBase = 0x70000000ull;
constexpr uint64_t kStackSize = 0x20000ull;
constexpr uint64_t kReturnStub = 0x7FFF0000ull;
constexpr uint64_t kWorkspace = 0x60000000ull;
constexpr uint64_t kSetup = 0x60000100ull;   // 0x240 bytes
constexpr uint64_t kResult = 0x60000800ull;  // 0x240 bytes
constexpr uint64_t kFraction = kResult + 0x30;
// The core needs setup + 0x1F8 to be a writable scratch pointer; it is not a game
// object. A zeroed 0x100-byte area in the workspace page is sufficient (verified
// 2026-11-25: a zero pointer makes the core hit zero_address_without_contract).
constexpr uint64_t kScratch1f8 = kWorkspace + 0xB00;  // 0x100 bytes
constexpr uint8_t kFullFractionWrite = 0xF;
constexpr uint64_t kGsSlot = 0x61000000ull;
constexpr uint64_t kSetupSize = 0x240;
constexpr uint64_t kShadowBytes = 0x20;
constexpr uint64_t kFrameMarker = 0x534852414D5A4C41ull;

uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t AlignDown(uint64_t value) { return value & ~(kPage - 1); }

bool HookInvalid(uc_engine* uc, uc_mem_type type, uint64_t address, int size, int64_t value,
                 void* user) {
    (void)uc;
    (void)size;
    (void)value;
    return static_cast<UnicornBackend*>(user)->OnFault(static_cast<int>(type), address);
}

void HookCode(uc_engine* uc, uint64_t address, uint32_t size, void* user) {
    (void)uc;
    (void)size;
    static_cast<UnicornBackend*>(user)->OnInstruction(address);
}

void HookResultWrite(uc_engine*, uc_mem_type, uint64_t address, int size, int64_t, void* user) {
    static_cast<UnicornBackend*>(user)->OnWrite(address, size);
}

}  // namespace

namespace {

// One FPGA handle for the whole process. The device is exclusive: a second
// VMMDLL_Initialize fails, so every engine shares this target.
struct SharedTarget {
    std::mutex mutex;
    int refs = 0;
    void* vmm = nullptr;
    uint32_t pid = 0;
    uint64_t base = 0;
    uint32_t imageSize = 0;
};

SharedTarget& Target() {
    static SharedTarget target;
    return target;
}

}  // namespace
UnicornBackend::UnicornBackend(UnicornConfig config) : config_(std::move(config)) {
    if (!config_.setupTemplatePath.empty()) {
        std::ifstream file(config_.setupTemplatePath, std::ios::binary);
        if (file) {
            setupTemplate_.resize(kSetupSize, 0);
            file.read(reinterpret_cast<char*>(setupTemplate_.data()),
                static_cast<std::streamsize>(kSetupSize));
            setupTemplate_.resize(static_cast<size_t>(file.gcount()));
        }
    }
}

UnicornBackend::~UnicornBackend() {
    ReleaseScene();
    CloseProcess();
}

bool UnicornBackend::OpenProcess() {
    if (vmm_) return true;
    SharedTarget& target = Target();
    std::lock_guard<std::mutex> lock(target.mutex);
    if (target.refs == 0) {
        char device[] = "fpga";
        char norefresh[] = "-norefresh";
        char* args[] = {const_cast<char*>(""), const_cast<char*>("-device"), device, norefresh};
        target.vmm = VMMDLL_Initialize(4, args);
        if (!target.vmm) {
            std::fprintf(stderr, "TRACE: VMMDLL_Initialize failed\n");
            return false;
        }
        char name[128] = {};
        std::snprintf(name, sizeof(name), "%s", config_.processName.c_str());
        DWORD pid = 0;
        if (!VMMDLL_PidGetFromName(static_cast<VMM_HANDLE>(target.vmm), name, &pid) || !pid) {
            std::fprintf(stderr, "TRACE: PidGetFromName(%s) failed\n", name);
            VMMDLL_Close(static_cast<VMM_HANDLE>(target.vmm));
            target.vmm = nullptr;
            return false;
        }
        target.pid = pid;
        target.base = VMMDLL_ProcessGetModuleBaseU(static_cast<VMM_HANDLE>(target.vmm), pid, name);
        if (!target.base) {
            std::fprintf(stderr, "TRACE: ProcessGetModuleBaseU failed\n");
            VMMDLL_Close(static_cast<VMM_HANDLE>(target.vmm));
            target.vmm = nullptr;
            return false;
        }
        PVMMDLL_MAP_MODULEENTRY entry = nullptr;
        if (VMMDLL_Map_GetModuleFromNameU(static_cast<VMM_HANDLE>(target.vmm), pid, name, &entry, 0) &&
            entry) {
            target.imageSize = entry->cbImageSize;
            VMMDLL_MemFree(entry);
        }
        if (!target.imageSize) {
            std::fprintf(stderr, "TRACE: module size unavailable\n");
            VMMDLL_Close(static_cast<VMM_HANDLE>(target.vmm));
            target.vmm = nullptr;
            return false;
        }
    }
    ++target.refs;
    vmm_ = target.vmm;
    pid_ = target.pid;
    base_ = target.base;
    imageSize_ = target.imageSize;
    return true;
}

void UnicornBackend::CloseProcess() {
    if (!vmm_) return;
    SharedTarget& target = Target();
    std::lock_guard<std::mutex> lock(target.mutex);
    vmm_ = nullptr;
    if (--target.refs <= 0) {
        target.refs = 0;
        if (target.vmm) {
            VMMDLL_Close(static_cast<VMM_HANDLE>(target.vmm));
            target.vmm = nullptr;
        }
        target.pid = 0;
        target.base = 0;
        target.imageSize = 0;
    }
    pid_ = 0;
    base_ = 0;
    imageSize_ = 0;
}
bool UnicornBackend::ReadTarget(uint64_t address, void* out, size_t size) {
    if (!vmm_) return false;
    DWORD actual = 0;
    const bool ok = VMMDLL_MemReadEx(static_cast<VMM_HANDLE>(vmm_), static_cast<DWORD>(pid_), address,
        static_cast<PBYTE>(out), static_cast<DWORD>(size), &actual, VMMDLL_FLAG_NOCACHE) != FALSE &&
        actual == size;
    return ok;
}

bool UnicornBackend::ReadPage(uint64_t page, uint8_t* out) {
    if (page && (reader_ ? reader_(page, out, kPage) : ReadTarget(page, out, kPage))) {
        ++stats_.pageFetches;
        return true;
    }
    ++stats_.pageReadFailures;
    stats_.lastFailedPage = page;
    return false;
}

// A page can be transiently unreadable when the target's working set has been trimmed
// (paged-out .text after an idle period or a level change). Retrying briefly costs at most
// a millisecond; a page that stays unreadable is not cached and the request fails with
// Error, so the next request tries again. Nothing here ever zero-fills a missing page.
bool UnicornBackend::FetchPageWithRetry(uint64_t page, uint8_t* out) {
    const uint32_t retries = config_.pageReadRetries;
    for (uint32_t attempt = 0;; ++attempt) {
        if (ReadPage(page, out)) return true;
        if (attempt >= retries) {
            pageFailure_ = true;
            return false;
        }
        ++stats_.pageReadRetries;
        if (config_.pageReadRetryDelayUs) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(config_.pageReadRetryDelayUs));
        }
    }
}

bool UnicornBackend::InImage(uint64_t address) const {
    return address >= base_ && address < base_ + imageSize_;
}

bool UnicornBackend::ContextAlive(uint64_t context) const {
    if (!context) return false;
    if (!reader_ && !vmm_) return false;
    uint64_t head = 0;
    auto* self = const_cast<UnicornBackend*>(this);
    const bool read = reader_ ? reader_(context, &head, sizeof(head))
                              : self->ReadTarget(context, &head, sizeof(head));
    if (!read) return false;
    return InImage(head);
}

void UnicornBackend::LoadThreads() {
    teb_ = 0;
    if (!vmm_) return;
    PVMMDLL_MAP_THREAD map = nullptr;
    if (!VMMDLL_Map_GetThread(static_cast<VMM_HANDLE>(vmm_), static_cast<DWORD>(pid_), &map) || !map) return;
    int best = -1;
    for (DWORD i = 0; i < map->cMap; ++i) {
        const auto& entry = map->pMap[i];
        if (!entry.vaTeb) continue;
        int score = 0;
        if (entry.bRunning) score += 2;
        if (entry.vaRIP >= base_ && entry.vaRIP < base_ + imageSize_) score += 4;
        if (entry.vaStackBaseUser && entry.vaStackLimitUser) score += 3;
        if (score > best) {
            best = score;
            teb_ = entry.vaTeb;
        }
    }
    VMMDLL_MemFree(map);
}

bool UnicornBackend::MapSynthetic(uint64_t page) {
    if (!uc_) return false;
    if (mapped_.count(page)) return true;
    if (uc_mem_map(reinterpret_cast<uc_engine*>(uc_), page, kPage, UC_PROT_ALL) != UC_ERR_OK) {
        return false;
    }
    mapped_.insert(page);
    ++stats_.mappedPages;
    return true;
}

bool UnicornBackend::EnsurePage(uint64_t page) {
    if (!uc_) return false;
    if (mapped_.count(page)) return true;

    std::vector<uint8_t> bytes(kPage, 0);
    if (sharedStore_) {
        // One DMA read serves every engine in the process without holding
        // sharedStore_->mutex across FetchPageWithRetry() / sleep / DMA.
        bool servedFromCache = false;
        {
            std::unique_lock<std::mutex> lock(sharedStore_->mutex);
            for (;;) {
                const auto it = sharedStore_->pages.find(page);
                if (it != sharedStore_->pages.end()) {
                    std::memcpy(bytes.data(), it->second.data(), kPage);
                    ++sharedStore_->stats.hits;
                    ++stats_.cacheHits;
                    servedFromCache = true;
                    break;
                }
                if (sharedStore_->inFlightPages.insert(page).second) {
                    break;
                }
                sharedStore_->cv.wait(lock, [&] {
                    return sharedStore_->inFlightPages.count(page) == 0 ||
                           sharedStore_->pages.count(page) != 0;
                });
            }
        }
        if (!servedFromCache) {
            bool fetched = false;
            try {
                fetched = FetchPageWithRetry(page, bytes.data());
            } catch (...) {
                std::lock_guard<std::mutex> lock(sharedStore_->mutex);
                sharedStore_->inFlightPages.erase(page);
                sharedStore_->cv.notify_all();
                throw;
            }
            {
                std::lock_guard<std::mutex> lock(sharedStore_->mutex);
                sharedStore_->inFlightPages.erase(page);
                if (fetched) {
                    sharedStore_->pages.emplace(page, bytes);
                    ++sharedStore_->stats.fetches;
                }
                sharedStore_->cv.notify_all();
            }
            if (!fetched) return false;
        }
    } else {
        const auto it = pageBytes_.find(page);
        if (it != pageBytes_.end()) {
            std::memcpy(bytes.data(), it->second.data(), kPage);
            ++stats_.cacheHits;
        } else {
            if (!FetchPageWithRetry(page, bytes.data())) return false;
            pageBytes_.emplace(page, bytes);
        }
    }
    const uint32_t pagePerms = InImage(page) ? UC_PROT_ALL : (UC_PROT_READ | UC_PROT_WRITE);
    if (uc_mem_map(reinterpret_cast<uc_engine*>(uc_), page, kPage, pagePerms) != UC_ERR_OK) {
        return false;
    }
    if (uc_mem_write(reinterpret_cast<uc_engine*>(uc_), page, bytes.data(), kPage) != UC_ERR_OK) {
        uc_mem_unmap(reinterpret_cast<uc_engine*>(uc_), page, kPage);
        return false;
    }
    mapped_.insert(page);
    ++stats_.mappedPages;
    return true;
}

bool UnicornBackend::OnFault(int type, uint64_t address) {
    ++stats_.faults;
    if (type == UC_MEM_FETCH_UNMAPPED && !InImage(address)) {
        stop_ = true;
        uc_emu_stop(reinterpret_cast<uc_engine*>(uc_));
        return false;
    }
    const uint64_t page = AlignDown(address);
    if (!EnsurePage(page)) {
        stop_ = true;
        uc_emu_stop(reinterpret_cast<uc_engine*>(uc_));
        return false;
    }
    touched_.insert(page);
    return true;
}

bool UnicornBackend::OnInstruction(uint64_t address) {
    ++instructionCount_;
    if (!InImage(address) || instructionCount_ > config_.instructionLimit) {
        stop_ = true;
        uc_emu_stop(reinterpret_cast<uc_engine*>(uc_));
    }
    return true;
}

void UnicornBackend::OnWrite(uint64_t address, int size) {
    // A single emulated write can cover several result fields.
    for (uint8_t i = 0; i < sizeof(float); ++i) {
        const uint64_t byte = kFraction + i;
        if (address <= byte && byte - address < static_cast<uint64_t>(size)) {
            fractionWritten_ |= static_cast<uint8_t>(1u << i);
        }
    }
}

bool UnicornBackend::PrepareSetup() {
    std::vector<uint8_t> bytes(kSetupSize, 0);
    if (!setupTemplate_.empty()) {
        std::memcpy(bytes.data(), setupTemplate_.data(),
            (std::min)(setupTemplate_.size(), bytes.size()));
    } else {
        // Template-free 0x240 layout. The field set mirrors the live 0x240 captures of
        // the game's own initializer (2026-11-21 setup dumps, re-checked 2026-11-25):
        //   +0x20   1.0f       scalar input read by the core; a zero here made the
        //                      query return before it wrote the result.
        //   +0x58   trace mask
        //   +0x60 / +0x90 / +0xC0 / +0x100
        //           four inline containers {ptr,count,cap}; count stays 0 because the
        //           product traces world geometry without entity filter entries.
        //   +0x1A0  world-trace callback (build constant)
        //   +0x1B8  ray_cmp (none)
        //   +0x1C0  gm/local object (none)
        //   +0x1E8  level/world object (must be non-zero, see UnicornConfig)
        //   +0x1F0  level context
        //   +0x1F8  second level object (see UnicornConfig)
        // 0x200..0x240 is core-owned output state; it stays zero on input and the core
        // writes it while tracing.
        const float one = 1.0f;
        std::memcpy(bytes.data() + 0x20, &one, sizeof(one));
        const uint64_t mask = config_.mask;
        std::memcpy(bytes.data() + 0x58, &mask, sizeof(mask));
        const uint64_t list0 = kSetup + 0x70, list1 = kSetup + 0xA0, list2 = kSetup + 0xD0,
                       list3 = kSetup + 0x110;
        const uint32_t cap4 = 0x80000004, cap8 = 0x80000008;
        std::memcpy(bytes.data() + 0x60, &list0, 8);
        std::memcpy(bytes.data() + 0x6C, &cap4, 4);
        std::memcpy(bytes.data() + 0x90, &list1, 8);
        std::memcpy(bytes.data() + 0x9C, &cap4, 4);
        std::memcpy(bytes.data() + 0xC0, &list2, 8);
        std::memcpy(bytes.data() + 0xCC, &cap4, 4);
        std::memcpy(bytes.data() + 0x100, &list3, 8);
        std::memcpy(bytes.data() + 0x10C, &cap8, 4);
        // Filter list: one entry excludes the local player's own body. Refresh it per
        // trace from the live local entity when a resolver is available, so a hero swap
        // cannot leave a stale parent pointer in the setup.
        uint64_t filterItem = config_.filterCount ? config_.filterItems[0] : 0;
        if (config_.filterItemResolver) filterItem = config_.filterItemResolver();
        if (filterItem) {
            const uint32_t count = 1;
            std::memcpy(bytes.data() + 0x68, &count, 4);
            std::memcpy(bytes.data() + 0x70, &filterItem, 8);
        }
        if (config_.localObject)
            std::memcpy(bytes.data() + 0x40, &config_.localObject, 8);
        // The core reads setup + 0x1E8 before the world query and skips the query entirely
        // when it is zero. Live captures always carry a heap object here.
        std::memcpy(bytes.data() + 0x1E8, &config_.filterObject, 8);
        if (config_.auxCallback) std::memcpy(bytes.data() + 0x1A0, &config_.auxCallback, 8);
        // setup + 0x1F8 is a scratch pointer (not a scene object). A zero pointer aborts
        // the core at zero_address_without_contract; a zeroed workspace area works.
        const uint64_t scene2 = config_.sceneObject2 ? config_.sceneObject2 : kScratch1f8;
        std::memcpy(bytes.data() + 0x1F8, &scene2, 8);
        std::memcpy(bytes.data() + 0x1C0, &config_.context2, 8);
        // Fields the game's setup_ray() always writes before the trace call.
        const uint32_t field1C8 = 0x10000;
        std::memcpy(bytes.data() + 0x1C8, &field1C8, sizeof(field1C8));
        bytes[0x1CD] = 1;
    }
    // Both paths point their inline lists at the emulated copy, so a template captured on
    // the game stack stays self-contained after the copy.
    const uint64_t list0 = kSetup + 0x70, list1 = kSetup + 0xA0, list2 = kSetup + 0xD0,
                   list3 = kSetup + 0x110;
    std::memcpy(bytes.data() + 0x60, &list0, 8);
    std::memcpy(bytes.data() + 0x90, &list1, 8);
    std::memcpy(bytes.data() + 0xC0, &list2, 8);
    std::memcpy(bytes.data() + 0x100, &list3, 8);
    const uint64_t context = context_ ? context_ : config_.context;
    if (context) std::memcpy(bytes.data() + 0x1F0, &context, 8);
    if (config_.sceneObject2 == 0) {
        std::vector<uint8_t> scratch(0x100, 0);
        uc_mem_write(reinterpret_cast<uc_engine*>(uc_), kScratch1f8, scratch.data(),
            scratch.size());
    }
    return uc_mem_write(reinterpret_cast<uc_engine*>(uc_), kSetup, bytes.data(), bytes.size()) ==
        UC_ERR_OK;
}

bool UnicornBackend::RefreshTouchedPages() {
    std::vector<uint8_t> bytes(kPage);
    for (uint64_t page : touched_) {
        if (!ReadPage(page, bytes.data())) return false;
        if (sharedStore_) {
            std::lock_guard<std::mutex> lock(sharedStore_->mutex);
            sharedStore_->pages[page] = bytes;
        }
        if (uc_mem_write(reinterpret_cast<uc_engine*>(uc_), page, bytes.data(), kPage) != UC_ERR_OK) {
            return false;
        }
        if (!sharedStore_) pageBytes_[page] = bytes;
    }
    return true;
}

bool UnicornBackend::BindScene(uint64_t sceneEpoch) {
    if (bound_) return sceneEpoch_ == sceneEpoch;
    if (reader_) {
        if (!base_ || !imageSize_) {
            std::fprintf(stderr, "TRACE: reader mode needs SetTarget(base, imageSize)\n");
            return false;
        }
    } else if (!OpenProcess()) {
        return false;
    }
    if (!setupTemplate_.empty()) {
        // Template stays loaded; the context is re-read from it when the caller supplied none.
        if (!config_.context && setupTemplate_.size() >= 0x1F8) {
            std::memcpy(&context_, setupTemplate_.data() + 0x1F0, 8);
        }
    }
    context_ = config_.context ? config_.context : context_;
    if (config_.context) context_ = config_.context;
    if (!context_) { std::fprintf(stderr, "TRACE: no context (template=%zu bytes)\n", setupTemplate_.size()); return false; }
    if (!ContextAlive(context_)) { std::fprintf(stderr, "TRACE: context 0x%llX is not alive\n", static_cast<unsigned long long>(context_)); return false; }

    if (!externalTarget_) LoadThreads();

    if (!uc_) {
        uc_engine* engine = nullptr;
        if (uc_open(UC_ARCH_X86, UC_MODE_64, &engine) != UC_ERR_OK) { std::fprintf(stderr, "TRACE: uc_open failed\n"); return false; }
        uc_ = reinterpret_cast<uc_struct*>(engine);
        uc_hook invalidHook = 0, codeHook = 0, resultHook = 0;
        if (uc_hook_add(engine, &invalidHook,
                UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                    UC_HOOK_MEM_FETCH_UNMAPPED,
                reinterpret_cast<void*>(HookInvalid), this, 1, 0) != UC_ERR_OK) {
            std::fprintf(stderr, "TRACE: invalid-memory hook failed\n");
            ReleaseScene();
            return false;
        }
        if (uc_hook_add(engine, &resultHook, UC_HOOK_MEM_WRITE,
                reinterpret_cast<void*>(HookResultWrite), this, kWorkspace,
                kFraction + sizeof(float) - 1) != UC_ERR_OK) {
            std::fprintf(stderr, "TRACE: result-write hook failed\n");
            ReleaseScene();
            return false;
        }
        if (!config_.skipCodeHook) {
            if (uc_hook_add(engine, &codeHook, UC_HOOK_CODE, reinterpret_cast<void*>(HookCode),
                    this, 1, 0) != UC_ERR_OK) {
                std::fprintf(stderr, "TRACE: code hook failed\n");
                ReleaseScene();
                return false;
            }
        }
        // Synthetic regions are mapped once per scene and never unmapped.
        for (uint64_t page = kStackBase; page < kStackBase + kStackSize; page += kPage) {
            if (!MapSynthetic(page)) {
                ReleaseScene();
                return false;
            }
        }
        if (!MapSynthetic(AlignDown(kReturnStub)) || !MapSynthetic(AlignDown(kWorkspace)) ||
            !MapSynthetic(AlignDown(kSetup)) || !MapSynthetic(AlignDown(kResult)) ||
            !MapSynthetic(AlignDown(kGsSlot))) {
            std::fprintf(stderr, "TRACE: synthetic region mapping failed\n");
            ReleaseScene();
            return false;
        }
        uint64_t gs = teb_ ? teb_ : kGsSlot;
        uc_reg_write(engine, UC_X86_REG_GS_BASE, &gs);
    }
    if (teb_) {
        EnsurePage(AlignDown(teb_));
        EnsurePage(AlignDown(teb_) + kPage);
        uint64_t peb = 0;
        if (ReadTarget(teb_ + 0x60, &peb, sizeof(peb)) && peb) EnsurePage(AlignDown(peb));
    }
    // Touch the wrapper page once so a trimmed .text is noticed at bind instead of on the
    // first request. Failure is not fatal here: the page may come back before the trace,
    // and Trace retries it and fails explicitly instead of executing zero-filled code.
    if (config_.entryRva) {
        const uint64_t entryPage = AlignDown(base_ + config_.entryRva);
        if (!EnsurePage(entryPage)) {
            std::fprintf(stderr, "TRACE: entry page 0x%llX unreadable at bind; trace will retry\n",
                static_cast<unsigned long long>(entryPage));
        }
        pageFailure_ = false;
    }
    bound_ = true;
    sceneEpoch_ = sceneEpoch;
    return true;
}

void UnicornBackend::ReleaseScene() {
    bound_ = false;
    sceneEpoch_ = 0;
    if (uc_) {
        uc_close(reinterpret_cast<uc_engine*>(uc_));
        uc_ = nullptr;
    }
    mapped_.clear();
    pageBytes_.clear();
    touched_.clear();
}

Status UnicornBackend::Trace(const Request& request, Result& result) {
    result = Result{};
    if (!bound_ || !uc_) return result.status;
    if (request.sceneEpoch != sceneEpoch_) return result.status = Status::InvalidScene;
    const uint64_t started = NowUs();
    pageFailure_ = false;

    if (config_.refreshTouchedPages && !RefreshTouchedPages()) {
        if (pageFailure_) ++stats_.pageUnavailableTraces;
        return result.status = Status::Error;
    }

    const uint64_t tSetup0 = NowUs();
    if (!PrepareSetup()) return result.status = Status::Error;

    std::vector<uint8_t> zero(kSetupSize, 0);
    if (uc_mem_write(reinterpret_cast<uc_engine*>(uc_), kResult, zero.data(), zero.size()) !=
        UC_ERR_OK) {
        return result.status = Status::Error;
    }
    uc_engine* engine = reinterpret_cast<uc_engine*>(uc_);
    const float fractionInit = 0.5f;
    const uint32_t flagsZero = 0, hitboxNone = 0xFFFFu;
    const uint64_t effectiveMask = request.mask != 0 ? request.mask : config_.mask;
    uc_mem_write(engine, kSetup + 0x00, &request.start.x, 4);
    uc_mem_write(engine, kSetup + 0x04, &request.start.y, 4);
    uc_mem_write(engine, kSetup + 0x08, &request.start.z, 4);
    uc_mem_write(engine, kSetup + 0x10, &request.end.x, 4);
    uc_mem_write(engine, kSetup + 0x14, &request.end.y, 4);
    uc_mem_write(engine, kSetup + 0x18, &request.end.z, 4);
    // Phase 3: honor per-request layer mask (world vs barrier) on every call, including
    // template setups and multi-mask concurrent VisionProbe waves.
    uc_mem_write(engine, kSetup + 0x58, &effectiveMask, sizeof(effectiveMask));
    uc_mem_write(engine, kFraction, &fractionInit, 4);
    uc_mem_write(engine, kResult + 0x28, &flagsZero, 4);
    uc_mem_write(engine, kResult + 0x38, &hitboxNone, 4);

    const uint64_t rsp = kStackBase + kStackSize - 0x80;
    const uint64_t returnAddress = kReturnStub;
    uint8_t shadow[kShadowBytes] = {};
    const uint64_t marker = kFrameMarker;
    uc_mem_write(engine, rsp, &returnAddress, 8);
    uc_mem_write(engine, rsp + 8, shadow, sizeof(shadow));
    uc_mem_write(engine, rsp + 8 + kShadowBytes, &marker, 8);

    const uint64_t tPrep0 = NowUs();
    const uint64_t entry = base_ + config_.entryRva;
    uint64_t rcx = kSetup, rdx = kResult, r8 = config_.flags, r9 = 0, rdi = kWorkspace;
    uint64_t gs = teb_ ? teb_ : kGsSlot;
    uc_reg_write(engine, UC_X86_REG_RSP, &rsp);
    uc_reg_write(engine, UC_X86_REG_RCX, &rcx);
    uc_reg_write(engine, UC_X86_REG_RDX, &rdx);
    uc_reg_write(engine, UC_X86_REG_R8, &r8);
    uc_reg_write(engine, UC_X86_REG_R9, &r9);
    uc_reg_write(engine, UC_X86_REG_RDI, &rdi);
    uc_reg_write(engine, UC_X86_REG_GS_BASE, &gs);

    stop_ = false;
    fractionWritten_ = 0;
    instructionCount_ = 0;
    const uint64_t tEmu0 = NowUs();
    const uc_err err = uc_emu_start(engine, entry, kReturnStub, config_.timeoutUs,
        static_cast<size_t>(config_.instructionLimit));
    stats_.emuUs = NowUs() - tEmu0;
    stats_.prepUs = tEmu0 - tPrep0;
    stats_.setupUs = tPrep0 - tSetup0;
    uint64_t rip = 0;
    uc_reg_read(engine, UC_X86_REG_RIP, &rip);
    stats_.instructions = instructionCount_;
    ++stats_.traces;
    stats_.lastUs = NowUs() - started;
    stats_.maxUs = (std::max)(stats_.maxUs, stats_.lastUs);
    result.latencyUs = static_cast<uint32_t>(std::min<uint64_t>(stats_.lastUs, 0xFFFFFFFFu));
    result.instructions = instructionCount_;

    if (err != UC_ERR_OK || stop_ || rip != kReturnStub || fractionWritten_ != kFullFractionWrite) {
        if (pageFailure_) ++stats_.pageUnavailableTraces;
        return result.status = Status::Error;
    }
    uint32_t flags = 0, hitbox = 0xFFFF, entity = 0xFFFF;
    float fraction = 1.0f;
    if (uc_mem_read(engine, kFraction, &fraction, sizeof(fraction)) != UC_ERR_OK ||
        uc_mem_read(engine, kResult + 0x28, &flags, sizeof(flags)) != UC_ERR_OK ||
        uc_mem_read(engine, kResult + 0x38, &hitbox, sizeof(hitbox)) != UC_ERR_OK ||
        uc_mem_read(engine, kResult + 0x3C, &entity, sizeof(entity)) != UC_ERR_OK ||
        !std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f) {
        return result.status = Status::Error;
    }

    result.fraction = fraction;
    result.hitFlags = flags;
    result.hitbox = hitbox;
    result.entityId = entity;

    // Phase 3: backfill hitPosition (+0x00) and hitNormal (+0x10) on blocking hits.
    // Layout statically verified across World 155284 (0xD0D891..0xD0D8C9), CN 155284N
    // (0x10213FD..0x102142D), and Nexon 155284KB (0x100B6FD..0x100B72D), and live-confirmed
    // on World 155284 blocked-native.log.
    if (fraction < 1.0f) {
        float rawPos[3] = {};
        float rawNorm[3] = {};
        if (uc_mem_read(engine, kResult + 0x00, rawPos, sizeof(rawPos)) == UC_ERR_OK &&
            std::isfinite(rawPos[0]) && std::isfinite(rawPos[1]) && std::isfinite(rawPos[2])) {
            result.hitPosition = {rawPos[0], rawPos[1], rawPos[2]};
            result.hasHitPosition = true;
        }
        if (uc_mem_read(engine, kResult + 0x10, rawNorm, sizeof(rawNorm)) == UC_ERR_OK &&
            std::isfinite(rawNorm[0]) && std::isfinite(rawNorm[1]) && std::isfinite(rawNorm[2])) {
            const float normSq = rawNorm[0] * rawNorm[0] +
                                 rawNorm[1] * rawNorm[1] +
                                 rawNorm[2] * rawNorm[2];
            if (std::isfinite(normSq) && normSq > 1e-6f) {
                const float invLen = 1.0f / std::sqrt(normSq);
                result.hitNormal = {rawNorm[0] * invLen, rawNorm[1] * invLen, rawNorm[2] * invLen};
                result.hasHitNormal = true;
            }
        }
    }
    return result.status = Status::Ok;
}

}  // namespace OW::EngineTrace











