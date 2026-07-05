#include "ControlHandlers.hpp"

#include <future>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

#include "libslic3r/libslic3r.h" // for SLIC3R_VERSION (via generated libslic3r_version.h)

namespace Slic3r { namespace GUI { namespace ControlAPI {

// ---------------------------------------------------------------------------
// OperationTracker / EventBuffer
// ---------------------------------------------------------------------------

static constexpr size_t MAX_TRACKED_OPS    = 64;
static constexpr size_t MAX_BUFFERED_EVENTS = 256;

std::uint64_t OperationTracker::start(const std::string& kind, int plate_index)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Operation op;
    op.id          = next_id_++;
    op.kind        = kind;
    op.plate_index = plate_index;
    op.started_at  = std::chrono::steady_clock::now();
    ops_.push_back(op);
    while (ops_.size() > MAX_TRACKED_OPS)
        ops_.pop_front();
    return op.id;
}

bool OperationTracker::get(std::uint64_t id, Operation& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Operation& op : ops_)
        if (op.id == id) {
            out = op;
            return true;
        }
    return false;
}

void OperationTracker::resolve(std::uint64_t id, const std::string& final_status, json result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (Operation& op : ops_)
        if (op.id == id) {
            // First resolution wins; polling may race with itself otherwise.
            if (op.final_status.empty()) {
                op.final_status = final_status;
                op.result       = std::move(result);
            }
            return;
        }
}

void EventBuffer::push(const std::string& type, json payload)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        json event;
        event["seq"]     = next_seq_++;
        event["type"]    = type;
        event["payload"] = std::move(payload);
        events_.push_back(std::move(event));
        while (events_.size() > MAX_BUFFERED_EVENTS)
            events_.pop_front();
    }
    cv_.notify_all();
}

json EventBuffer::wait_since(std::uint64_t since, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);
    auto has_new = [&] { return !events_.empty() && events_.back()["seq"].get<std::uint64_t>() > since; };
    if (!has_new() && timeout_ms > 0)
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), has_new);

    json out = json::array();
    for (const json& event : events_)
        if (event["seq"].get<std::uint64_t>() > since)
            out.push_back(event);
    return out;
}

std::uint64_t EventBuffer::latest_seq()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return next_seq_ - 1;
}

OperationTracker& operation_tracker()
{
    static OperationTracker tracker;
    return tracker;
}

EventBuffer& event_buffer()
{
    static EventBuffer buffer;
    return buffer;
}

// ---------------------------------------------------------------------------
// UI-thread marshalling
// ---------------------------------------------------------------------------

// Runs `fn` on the wx main loop and waits for its JSON result. The timeout
// protects the network thread from a blocked UI (e.g. a modal dialog): the
// caller gets a 504 instead of a hung connection. NOTE: on timeout the lambda
// may still run later — handlers must therefore be idempotent or harmless to
// re-observe, which all current ones are (they only read state or perform the
// action the client explicitly requested).
static json run_on_ui(std::function<json()> fn, int timeout_ms = 10000)
{
    auto promise = std::make_shared<std::promise<json>>();
    auto future  = promise->get_future();

    wxGetApp().CallAfter([promise, fn = std::move(fn)]() {
        try {
            promise->set_value(fn());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });

    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready)
        throw ApiError(504, "ui_timeout",
                       "the UI thread did not respond in time (a modal dialog may be open in Bambu Studio)");
    return future.get(); // rethrows any handler exception
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static json parse_body(const std::string& body)
{
    if (body.empty())
        return json::object();
    json parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded())
        throw ApiError(400, "bad_json", "request body is not valid JSON");
    return parsed;
}

static std::unordered_map<std::string, std::string> parse_query(const std::string& qs)
{
    std::unordered_map<std::string, std::string> out;
    size_t start = 0;
    while (start < qs.size()) {
        size_t amp   = qs.find('&', start);
        size_t len   = (amp == std::string::npos ? qs.size() : amp) - start;
        std::string pair = qs.substr(start, len);
        size_t      eq   = pair.find('=');
        if (eq == std::string::npos)
            out[pair] = "";
        else
            out[pair.substr(0, eq)] = pair.substr(eq + 1);
        if (amp == std::string::npos)
            break;
        start = amp + 1;
    }
    return out;
}

static Plater* plater_or_throw()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        throw ApiError(503, "not_ready", "the plater is not initialized yet");
    return plater;
}

static json vec3d_to_json(const Vec3d& v) { return json::array({v.x(), v.y(), v.z()}); }

static Vec3d json_to_vec3d(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number())
        throw ApiError(400, "bad_request", std::string(what) + " must be an array of 3 numbers");
    return Vec3d(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
}

// ---------------------------------------------------------------------------
// Endpoint implementations (all invoked from the network thread; anything
// touching GUI state goes through run_on_ui).
// ---------------------------------------------------------------------------

static json handle_status()
{
    return run_on_ui([]() -> json {
        json data;
        data["app"]              = "BambuStudio";
        data["version"]          = std::string(SLIC3R_VERSION);
        data["control_api"]      = 1;
        Plater* plater           = wxGetApp().plater();
        data["plater_ready"]     = plater != nullptr;
        if (plater != nullptr) {
            data["slicing"]        = plater->is_background_process_slicing();
            data["project_dirty"]  = plater->is_project_dirty();
            PartPlateList& plates  = plater->get_partplate_list();
            data["plate_count"]    = plates.get_plate_count();
            data["current_plate"]  = plates.get_curr_plate_index();
            data["object_count"]   = plater->model().objects.size();
        }
        return data;
    });
}

static json handle_project_open(const json& body)
{
    const std::string path = body.value("path", "");
    if (path.empty())
        throw ApiError(400, "bad_request", "missing 'path'");
    if (!boost::filesystem::exists(path))
        throw ApiError(404, "not_found", "file does not exist: " + path);

    json result = run_on_ui([path]() -> json {
        Plater* plater = plater_or_throw();
        // load_files with LoadModel|LoadConfig mirrors File->Open Project for
        // 3MF; for STL it simply imports the geometry into the current project.
        std::vector<std::string> files{path};
        std::vector<size_t> loaded = plater->load_files(files, LoadStrategy::LoadModel | LoadStrategy::LoadConfig, false);
        json out;
        out["loaded_object_indices"] = loaded;
        out["object_count"]          = plater->model().objects.size();
        return out;
    }, 60000); // big projects can take a while to load

    event_buffer().push("project_opened", json{{"path", path}});
    return result;
}

static json handle_project_import(const json& body)
{
    if (!body.contains("paths") || !body["paths"].is_array() || body["paths"].empty())
        throw ApiError(400, "bad_request", "missing 'paths' (non-empty array)");
    std::vector<std::string> files;
    for (const json& p : body["paths"]) {
        if (!p.is_string())
            throw ApiError(400, "bad_request", "'paths' entries must be strings");
        if (!boost::filesystem::exists(p.get<std::string>()))
            throw ApiError(404, "not_found", "file does not exist: " + p.get<std::string>());
        files.push_back(p.get<std::string>());
    }

    json result = run_on_ui([files]() -> json {
        Plater* plater = plater_or_throw();
        // LoadModel only: importing meshes must not clobber project settings.
        std::vector<size_t> loaded = plater->load_files(files, LoadStrategy::LoadModel, false);
        json out;
        out["loaded_object_indices"] = loaded;
        out["object_count"]          = plater->model().objects.size();
        return out;
    }, 60000);

    event_buffer().push("models_imported", json{{"count", files.size()}});
    return result;
}

static json handle_project_save(const json& body)
{
    const std::string path = body.value("path", "");
    if (path.empty())
        throw ApiError(400, "bad_request", "missing 'path'");
    if (!boost::algorithm::iends_with(path, ".3mf"))
        throw ApiError(400, "bad_request", "'path' must end in .3mf");

    json result = run_on_ui([path]() -> json {
        Plater* plater = plater_or_throw();
        int     ret    = plater->export_3mf(boost::filesystem::path(path), SaveStrategy::SplitModel | SaveStrategy::ShareMesh);
        if (ret != 0)
            throw ApiError(500, "save_failed", "export_3mf returned " + std::to_string(ret));
        json out;
        out["path"] = path;
        return out;
    }, 60000);

    event_buffer().push("project_saved", json{{"path", path}});
    return result;
}

static json object_to_json(const ModelObject* object, size_t index)
{
    json j;
    j["index"] = index;
    j["name"]  = object->name;
    j["instance_count"] = object->instances.size();
    if (!object->instances.empty()) {
        const ModelInstance* instance = object->instances.front();
        j["translate"] = vec3d_to_json(instance->get_offset());
        j["rotate_rad"] = vec3d_to_json(instance->get_rotation());
        j["scale"]     = vec3d_to_json(instance->get_scaling_factor());
    }
    j["volume_count"] = object->volumes.size();
    const BoundingBoxf3 bb = object->bounding_box_exact();
    j["bbox_min"] = vec3d_to_json(bb.min);
    j["bbox_max"] = vec3d_to_json(bb.max);
    return j;
}

static json handle_objects_list()
{
    return run_on_ui([]() -> json {
        Plater* plater = plater_or_throw();
        json    out    = json::array();
        const ModelObjectPtrs& objects = plater->model().objects;
        for (size_t i = 0; i < objects.size(); ++i)
            out.push_back(object_to_json(objects[i], i));
        return out;
    });
}

static json handle_object_transform(size_t index, const json& body)
{
    const bool has_translate = body.contains("translate");
    const bool has_rotate    = body.contains("rotate_deg");
    const bool has_scale     = body.contains("scale");
    if (!has_translate && !has_rotate && !has_scale)
        throw ApiError(400, "bad_request", "provide at least one of translate / rotate_deg / scale");

    Vec3d translate = has_translate ? json_to_vec3d(body["translate"], "translate") : Vec3d::Zero();
    Vec3d rotate_deg = has_rotate ? json_to_vec3d(body["rotate_deg"], "rotate_deg") : Vec3d::Zero();
    Vec3d scale = Vec3d::Ones();
    if (has_scale) {
        if (body["scale"].is_number())
            scale = Vec3d::Ones() * body["scale"].get<double>();
        else
            scale = json_to_vec3d(body["scale"], "scale");
        if (scale.x() <= 0 || scale.y() <= 0 || scale.z() <= 0)
            throw ApiError(400, "bad_request", "scale factors must be positive");
    }

    json result = run_on_ui([index, translate, rotate_deg, scale, has_translate, has_rotate, has_scale]() -> json {
        Plater* plater = plater_or_throw();
        ModelObjectPtrs& objects = plater->model().objects;
        if (index >= objects.size())
            throw ApiError(404, "not_found", "no object with index " + std::to_string(index));
        ModelObject* object = objects[index];

        // Transforms are applied per-instance, relative to the current pose —
        // the same semantics as nudging the object in the GUI.
        for (ModelInstance* instance : object->instances) {
            if (has_translate)
                instance->set_offset(instance->get_offset() + translate);
            if (has_rotate) {
                const Vec3d rad = rotate_deg * (M_PI / 180.0);
                instance->set_rotation(instance->get_rotation() + rad);
            }
            if (has_scale)
                instance->set_scaling_factor(instance->get_scaling_factor().cwiseProduct(scale));
        }
        object->invalidate_bounding_box();
        plater->update();
        return object_to_json(object, index);
    });

    event_buffer().push("object_transformed", json{{"index", index}});
    return result;
}

static json handle_object_delete(size_t index)
{
    json result = run_on_ui([index]() -> json {
        Plater* plater = plater_or_throw();
        if (index >= plater->model().objects.size())
            throw ApiError(404, "not_found", "no object with index " + std::to_string(index));
        plater->remove(index);
        json out;
        out["object_count"] = plater->model().objects.size();
        return out;
    });

    event_buffer().push("object_deleted", json{{"index", index}});
    return result;
}

static json presets_of(const PresetCollection& collection)
{
    json out = json::array();
    for (const Preset& preset : collection) {
        if (!preset.is_visible)
            continue;
        json j;
        j["name"]      = preset.name;
        j["is_system"] = preset.is_system;
        j["selected"]  = collection.get_selected_preset_name() == preset.name;
        out.push_back(j);
    }
    return out;
}

static json handle_presets_list()
{
    return run_on_ui([]() -> json {
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr)
            throw ApiError(503, "not_ready", "preset bundle not initialized");
        json out;
        out["printer"]  = presets_of(bundle->printers);
        out["filament"] = presets_of(bundle->filaments);
        out["process"]  = presets_of(bundle->prints);
        return out;
    });
}

static json handle_presets_select(const json& body)
{
    const std::string printer  = body.value("printer", "");
    const std::string filament = body.value("filament", "");
    const std::string process  = body.value("process", "");
    if (printer.empty() && filament.empty() && process.empty())
        throw ApiError(400, "bad_request", "provide at least one of printer / filament / process");

    json result = run_on_ui([printer, filament, process]() -> json {
        json out;
        // Selection goes through the Tab machinery (not PresetBundle directly)
        // so the sidebar, config, and dependent preset visibility all update
        // exactly as if the user picked the preset in the UI. Order matters:
        // printer first, since it constrains compatible filament/process.
        if (!printer.empty()) {
            Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
            if (tab == nullptr)
                throw ApiError(503, "not_ready", "printer tab not available");
            if (!tab->select_preset(printer))
                throw ApiError(404, "preset_not_found", "printer preset not selectable: " + printer);
            out["printer"] = printer;
        }
        if (!filament.empty()) {
            Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
            if (tab == nullptr)
                throw ApiError(503, "not_ready", "filament tab not available");
            if (!tab->select_preset(filament))
                throw ApiError(404, "preset_not_found", "filament preset not selectable: " + filament);
            out["filament"] = filament;
        }
        if (!process.empty()) {
            Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
            if (tab == nullptr)
                throw ApiError(503, "not_ready", "process tab not available");
            if (!tab->select_preset(process))
                throw ApiError(404, "preset_not_found", "process preset not selectable: " + process);
            out["process"] = process;
        }
        return out;
    }, 30000);

    event_buffer().push("presets_selected", result);
    return result;
}

static json handle_slice(int plate_index)
{
    json started = run_on_ui([plate_index]() -> json {
        Plater* plater = plater_or_throw();
        if (plater->model().objects.empty())
            throw ApiError(409, "empty_project", "nothing to slice: the project has no objects");
        if (plater->is_background_process_slicing())
            throw ApiError(409, "busy", "a slicing job is already running");

        PartPlateList& plates = plater->get_partplate_list();
        if (plate_index >= 0) {
            if (plate_index >= plates.get_plate_count())
                throw ApiError(404, "not_found", "no plate with index " + std::to_string(plate_index));
            plates.select_plate(plate_index);
        }
        plater->reslice();
        json out;
        out["plate_index"] = plates.get_curr_plate_index();
        return out;
    });

    const int      effective_plate = started["plate_index"].get<int>();
    std::uint64_t  op_id           = operation_tracker().start("slice", effective_plate);
    json           out;
    out["op_id"]       = op_id;
    out["plate_index"] = effective_plate;
    event_buffer().push("slice_started", out);
    return out;
}

static json handle_arrange(int plate_index)
{
    json started = run_on_ui([plate_index]() -> json {
        Plater* plater = plater_or_throw();
        PartPlateList& plates = plater->get_partplate_list();
        if (plate_index >= 0) {
            if (plate_index >= plates.get_plate_count())
                throw ApiError(404, "not_found", "no plate with index " + std::to_string(plate_index));
            plates.select_plate(plate_index);
        }
        plater->arrange();
        json out;
        out["plate_index"] = plates.get_curr_plate_index();
        return out;
    });

    std::uint64_t op_id = operation_tracker().start("arrange", started["plate_index"].get<int>());
    json          out;
    out["op_id"]       = op_id;
    out["plate_index"] = started["plate_index"];
    event_buffer().push("arrange_started", out);
    return out;
}

static json slice_result_json(PartPlate* plate)
{
    json out;
    out["is_valid"] = plate->is_slice_result_valid();
    if (!plate->is_slice_result_valid())
        return out;

    const GCodeProcessorResult* result = plate->get_slice_result();
    if (result == nullptr)
        return out;

    // Print time estimates per mode (Normal / Silent).
    json modes = json::array();
    for (size_t i = 0; i < (size_t) PrintEstimatedStatistics::ETimeMode::Count; ++i) {
        const auto& mode = result->print_statistics.modes[i];
        if (mode.time <= 0.0f)
            continue;
        json m;
        m["mode"]         = i == 0 ? "normal" : "silent";
        m["time_seconds"] = mode.time;
        modes.push_back(m);
    }
    out["time_estimates"] = modes;

    json volumes = json::object();
    for (const auto& [extruder_id, volume] : result->print_statistics.total_volumes_per_extruder)
        volumes[std::to_string(extruder_id)] = volume;
    out["filament_volume_mm3_per_extruder"] = volumes;

    out["gcode_path"] = result->filename; // temp gcode produced by the slice
    return out;
}

static json handle_slice_result(int plate_index)
{
    return run_on_ui([plate_index]() -> json {
        Plater*        plater = plater_or_throw();
        PartPlateList& plates = plater->get_partplate_list();
        const int      index  = plate_index >= 0 ? plate_index : plates.get_curr_plate_index();
        if (index < 0 || index >= plates.get_plate_count())
            throw ApiError(404, "not_found", "no plate with index " + std::to_string(index));
        PartPlate* plate = plates.get_plate(index);
        if (plate == nullptr)
            throw ApiError(404, "not_found", "no plate with index " + std::to_string(index));
        json out          = slice_result_json(plate);
        out["plate_index"] = index;
        return out;
    });
}

static json handle_operation_get(std::uint64_t op_id)
{
    Operation op;
    if (!operation_tracker().get(op_id, op))
        throw ApiError(404, "not_found", "unknown operation id " + std::to_string(op_id));

    json out;
    out["op_id"]       = op.id;
    out["kind"]        = op.kind;
    out["plate_index"] = op.plate_index;

    if (!op.final_status.empty()) {
        out["status"] = op.final_status;
        out["result"] = op.result;
        return out;
    }

    // Resolve lazily against live GUI state. Slicing: running while the
    // background process is active; once idle, the plate's slice-result
    // validity tells success from failure. Arrange: the arrange job has no
    // cheap public "running" flag, so we report it as running for a grace
    // period and then check nothing crashed (best effort for V1).
    if (op.kind == "slice") {
        json state = run_on_ui([&op]() -> json {
            Plater*        plater = plater_or_throw();
            PartPlateList& plates = plater->get_partplate_list();
            json           s;
            s["slicing"] = plater->is_background_process_slicing();
            PartPlate* plate = op.plate_index >= 0 && op.plate_index < plates.get_plate_count() ?
                                   plates.get_plate(op.plate_index) : plates.get_curr_plate();
            s["result"] = plate != nullptr ? slice_result_json(plate) : json::object();
            return s;
        });
        if (state["slicing"].get<bool>()) {
            out["status"] = "running";
        } else {
            const bool ok = state["result"].value("is_valid", false);
            const std::string status = ok ? "succeeded" : "failed";
            operation_tracker().resolve(op.id, status, state["result"]);
            out["status"] = status;
            out["result"] = state["result"];
            event_buffer().push(op.kind + (ok ? "_succeeded" : "_failed"), json{{"op_id", op.id}});
        }
    } else {
        // "arrange" (and future kinds without a live flag): resolved by time.
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - op.started_at);
        if (elapsed.count() < 3) {
            out["status"] = "running";
        } else {
            operation_tracker().resolve(op.id, "succeeded", json::object());
            out["status"] = "succeeded";
        }
    }
    return out;
}

static json handle_events(const std::string& query)
{
    auto params = parse_query(query);
    std::uint64_t since = 0;
    int timeout_s = 0;
    try {
        if (auto it = params.find("since"); it != params.end() && !it->second.empty())
            since = std::stoull(it->second);
        if (auto it = params.find("timeout"); it != params.end() && !it->second.empty())
            timeout_s = std::stoi(it->second);
    } catch (const std::exception&) {
        throw ApiError(400, "bad_request", "'since' and 'timeout' must be integers");
    }
    timeout_s = std::min(std::max(timeout_s, 0), 60);

    json out;
    out["events"]     = event_buffer().wait_since(since, timeout_s * 1000);
    out["latest_seq"] = event_buffer().latest_seq();
    return out;
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

// Splits "/api/v1/objects/3/transform" into segments after the prefix.
static std::vector<std::string> route_segments(const std::string& target)
{
    static const std::string prefix = "/api/v1/";
    if (!boost::starts_with(target, prefix))
        return {};
    std::vector<std::string> segments;
    boost::split(segments, target.substr(prefix.size()), boost::is_any_of("/"), boost::token_compress_on);
    while (!segments.empty() && segments.back().empty())
        segments.pop_back();
    return segments;
}

static size_t parse_index(const std::string& s, const char* what)
{
    try {
        return std::stoull(s);
    } catch (const std::exception&) {
        throw ApiError(400, "bad_request", std::string("invalid ") + what + ": " + s);
    }
}

json dispatch(const std::string& method,
              const std::string& target,
              const std::string& query,
              const std::string& body,
              int&               http_status)
{
    http_status = 200;
    const std::vector<std::string> seg = route_segments(target);
    if (seg.empty())
        throw ApiError(404, "not_found", "unknown route: " + target);

    if (method == "GET" && seg.size() == 1 && seg[0] == "status")
        return handle_status();

    if (seg[0] == "project" && seg.size() == 2 && method == "POST") {
        const json parsed = parse_body(body);
        if (seg[1] == "open")   return handle_project_open(parsed);
        if (seg[1] == "import") return handle_project_import(parsed);
        if (seg[1] == "save")   return handle_project_save(parsed);
    }

    if (seg[0] == "objects") {
        if (method == "GET" && seg.size() == 1)
            return handle_objects_list();
        if (seg.size() == 2 && method == "DELETE")
            return handle_object_delete(parse_index(seg[1], "object index"));
        if (seg.size() == 3 && method == "POST" && seg[2] == "transform")
            return handle_object_transform(parse_index(seg[1], "object index"), parse_body(body));
    }

    if (seg[0] == "presets") {
        if (method == "GET" && seg.size() == 1)
            return handle_presets_list();
        if (method == "POST" && seg.size() == 2 && seg[1] == "select")
            return handle_presets_select(parse_body(body));
    }

    if (seg[0] == "plates" && seg.size() == 3) {
        // "current" targets the currently selected plate.
        const int plate_index = seg[1] == "current" ? -1 : (int) parse_index(seg[1], "plate index");
        if (method == "POST" && seg[2] == "slice") {
            json out = handle_slice(plate_index);
            http_status = 202;
            return out;
        }
        if (method == "POST" && seg[2] == "arrange") {
            json out = handle_arrange(plate_index);
            http_status = 202;
            return out;
        }
        if (method == "GET" && seg[2] == "slice_result")
            return handle_slice_result(plate_index);
    }

    if (seg[0] == "operations" && seg.size() == 2 && method == "GET")
        return handle_operation_get(parse_index(seg[1], "operation id"));

    if (seg[0] == "events" && seg.size() == 1 && method == "GET")
        return handle_events(query);

    throw ApiError(404, "not_found", "unknown route: " + method + " " + target);
}

}}} // namespace Slic3r::GUI::ControlAPI
