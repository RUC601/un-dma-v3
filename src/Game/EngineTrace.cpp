#include "Game/EngineTrace.hpp"

#include <chrono>

namespace OW::EngineTrace {
namespace {

// Stand-in backend. It answers "unavailable" and never invents a visibility result.
class NullBackend final : public IBackend {
public:
    const char* Name() const override { return "null"; }
    bool BindScene(uint64_t) override { return false; }
    Status Trace(const Request&, Result&) override { return Status::Unavailable; }
    void ReleaseScene() override {}
};

NullBackend& Null() {
    static NullBackend backend;
    return backend;
}

uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

Service::Service(IBackend* backend) : backend_(backend ? backend : &Null()) {}

Service::~Service() = default;

void Service::SetBackend(IBackend* backend) {
    if (backend_ && backend_ != &Null()) backend_->ReleaseScene();
    backend_ = backend ? backend : &Null();
    bound_ = false;
    boundEpoch_ = 0;
}

bool Service::OnSceneChanged(uint64_t sceneEpoch) {
    if (backend_ && backend_ != &Null()) backend_->ReleaseScene();
    bound_ = false;
    boundEpoch_ = 0;
    if (!backend_ || backend_ == &Null()) return false;
    if (!backend_->BindScene(sceneEpoch)) return false;
    bound_ = true;
    boundEpoch_ = sceneEpoch;
    return true;
}

Result Service::Trace(const Request& request) {
    Result result;
    ++stats_.requests;

    if (!backend_ || backend_ == &Null()) {
        result.status = Status::Unavailable;
        ++stats_.unavailable;
        return result;
    }
    if (!bound_ || request.sceneEpoch != boundEpoch_) {
        result.status = Status::InvalidScene;
        ++stats_.invalidScene;
        return result;
    }

    const uint64_t started = NowUs();
    const Status status = backend_->Trace(request, result);
    const uint32_t elapsed = static_cast<uint32_t>(NowUs() - started);
    result.status = status;
    result.latencyUs = elapsed;
    stats_.lastLatencyUs = elapsed;
    if (elapsed > stats_.maxLatencyUs) stats_.maxLatencyUs = elapsed;
    if (request.budgetMs && elapsed > request.budgetMs * 1000u) {
        result.overBudget = true;
        ++stats_.overBudget;
    }

    switch (status) {
        case Status::Ok: ++stats_.ok; break;
        case Status::Unavailable: ++stats_.unavailable; break;
        case Status::Timeout: ++stats_.timeouts; break;
        case Status::InvalidScene: ++stats_.invalidScene; break;
        case Status::Error: ++stats_.errors; break;
    }
    return result;
}

}  // namespace OW::EngineTrace
