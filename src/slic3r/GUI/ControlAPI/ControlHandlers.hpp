#ifndef slic3r_ControlAPI_ControlHandlers_hpp_
#define slic3r_ControlAPI_ControlHandlers_hpp_

// Endpoint implementations for the local Control API (see ControlServer.hpp).
//
// Threading contract: every function in the Handlers namespace is called on
// the network thread. Anything touching Plater / Model / PresetBundle / Tabs
// must go through run_on_ui() which posts to the wx main loop and waits with a
// timeout. Long-running operations (slice, arrange) are started on the UI
// thread and tracked in the OperationTracker; their status is answered by
// polling live GUI state, not by binding into Plater's internal events — this
// keeps the module's footprint outside ControlAPI/ to two GUI_App hooks.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI { namespace ControlAPI {

using nlohmann::json;

// Thrown by handlers to produce a structured error response.
class ApiError : public std::runtime_error
{
public:
    ApiError(int http_status, std::string code, const std::string& message)
        : std::runtime_error(message), http_status_(http_status), code_(std::move(code))
    {}
    int                http_status() const { return http_status_; }
    const std::string& code() const { return code_; }

private:
    int         http_status_;
    std::string code_;
};

// ---------------------------------------------------------------------------
// Async operation registry. Operations are identified by a monotonically
// increasing id. Status is resolved lazily: get_status() inspects live GUI
// state (e.g. "is the background process still slicing?") at query time.
// ---------------------------------------------------------------------------
struct Operation
{
    std::uint64_t id = 0;
    std::string   kind;       // "slice" | "arrange"
    int           plate_index = -1;
    std::chrono::steady_clock::time_point started_at;
    // Filled once resolved; empty string means "not resolved yet".
    std::string   final_status;   // "succeeded" | "failed"
    json          result;         // stats payload once succeeded
};

class OperationTracker
{
public:
    std::uint64_t start(const std::string& kind, int plate_index);
    // Returns nullptr if the id is unknown. The returned copy is a snapshot.
    bool          get(std::uint64_t id, Operation& out);
    void          resolve(std::uint64_t id, const std::string& final_status, json result);

private:
    std::mutex                 mutex_;
    std::uint64_t              next_id_ = 1;
    std::deque<Operation>      ops_;    // bounded history, oldest dropped
};

// ---------------------------------------------------------------------------
// Event ring buffer with long-poll support. V1 emits events for actions taken
// through this API (project_opened, slice_started, ...); observing user-driven
// GUI actions is a later increment.
// ---------------------------------------------------------------------------
class EventBuffer
{
public:
    void push(const std::string& type, json payload);
    // Blocks up to timeout_ms for an event with seq > since. Returns all
    // buffered events newer than `since` (possibly empty on timeout).
    json wait_since(std::uint64_t since, int timeout_ms);
    std::uint64_t latest_seq();

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::uint64_t           next_seq_ = 1;
    std::deque<json>        events_;   // bounded, oldest dropped
};

OperationTracker& operation_tracker();
EventBuffer&      event_buffer();

// ---------------------------------------------------------------------------
// Router entry point, called by ControlSession on the network thread.
// `method` is "GET"/"POST"/"DELETE", `target` the path without query string,
// `query` the raw query string (may be empty), `body` the request body.
// Returns the response JSON; sets http_status. Throws ApiError for errors
// (converted to the error envelope by the caller).
// ---------------------------------------------------------------------------
json dispatch(const std::string& method,
              const std::string& target,
              const std::string& query,
              const std::string& body,
              int&               http_status);

}}} // namespace Slic3r::GUI::ControlAPI

#endif // slic3r_ControlAPI_ControlHandlers_hpp_
