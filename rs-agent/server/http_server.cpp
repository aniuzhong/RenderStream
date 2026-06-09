#include "http_server.h"

#include <cctype>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <winsock2.h>
#include <Windows.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "process_manager.h"
#include "pipe_server.h"
#include "schema.h"

#include "utils/encoding.h"

using namespace std::chrono_literals;

namespace {

std::string UrlDecode(const std::string& src) {
    std::string result;
    result.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%' && i + 2 < src.size() &&
            std::isxdigit(static_cast<unsigned char>(src[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(src[i + 2]))) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return 0;
            };
            result += static_cast<char>((hex(src[i + 1]) << 4) | hex(src[i + 2]));
            i += 2;
        } else if (src[i] == '+') {
            result += ' ';
        } else {
            result += src[i];
        }
    }
    return result;
}

}  // namespace

HttpServer::HttpServer(asio::io_context& io, ProcessManager& pm, PipeServer& ps, int port)
    : io_(io)
    , pm_(pm)
    , ps_(ps)
    , port_(port)
{
    auto router = std::make_unique<router_t>();
    RegisterRoutes(*router);

    server_ = std::make_unique<server_t>(
        restinio::external_io_context(io_),
        restinio::server_settings_t<traits_t>{}
            .port(static_cast<std::uint16_t>(port))
            .address("0.0.0.0")
            .request_handler(std::move(router)));
}

HttpServer::~HttpServer() {
    Close();
}

void HttpServer::Open() {
    asio::post(io_, [this] { server_->open_sync(); });
}

void HttpServer::Close() {
    if (server_)
        server_->close_sync();
}

void HttpServer::RegisterRoutes(router_t& router) {
    using namespace restinio;

    // Helper: create a JSON response with proper Content-Type
    static const auto json_response = [](const request_handle_t& req, std::string body) {
        return req->create_response()
            .append_header(http_field::content_type, "application/json")
            .set_body(std::move(body))
            .done();
    };

    // GET /api/health
    router.http_get("/api/health",
        [](const request_handle_t& req, router::route_params_t) {
            return json_response(req, "{\"status\":\"ok\"}");
        });

    // GET /api/node/info
    router.http_get("/api/node/info",
        [](const request_handle_t& req, router::route_params_t) {
            nlohmann::json j;

            wchar_t hostname[256]{};
            DWORD len = 256;
            GetComputerNameW(hostname, &len);
            j["hostname"] = rs::utils::wstring_to_utf8(hostname);

            j["virtual_screen"]["x"] = GetSystemMetrics(SM_XVIRTUALSCREEN);
            j["virtual_screen"]["y"] = GetSystemMetrics(SM_YVIRTUALSCREEN);
            j["virtual_screen"]["w"] = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            j["virtual_screen"]["h"] = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            nlohmann::json displays = nlohmann::json::array();
            EnumDisplayMonitors(nullptr, nullptr,
                [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
                    auto* arr = reinterpret_cast<nlohmann::json*>(lParam);
                    MONITORINFOEXW mi{sizeof(mi)};
                    GetMonitorInfoW(hMon, &mi);
                    nlohmann::json d;
                    d["index"] = arr->size();
                    d["x"] = mi.rcMonitor.left;
                    d["y"] = mi.rcMonitor.top;
                    d["w"] = mi.rcMonitor.right - mi.rcMonitor.left;
                    d["h"] = mi.rcMonitor.bottom - mi.rcMonitor.top;
                    d["primary"] = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                    d["name"] = rs::utils::wstring_to_utf8(mi.szDevice);
                    arr->push_back(d);
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&displays));
            j["displays"] = displays;

            return json_response(req, j.dump());
        });

    // GET /api/unreal/list
    router.http_get("/api/unreal/list",
        [this](const request_handle_t& req, router::route_params_t) {
            auto pids = pm_.List();
            nlohmann::json arr = nlohmann::json::array();
            for (DWORD pid : pids)
                arr.push_back({{"pid", pid}});
            return json_response(req, arr.dump());
        });

    // POST /api/unreal/kill
    router.http_post("/api/unreal/kill",
        [this](const request_handle_t& req, router::route_params_t) {
            try {
                auto j = nlohmann::json::parse(req->body());
                DWORD pid = j.value("pid", 0u);
                if (pid == 0) {
                    return json_response(req, nlohmann::json{{"error", "field 'pid' is required"}}.dump());
                }

                if (!pm_.Kill(pid)) {
                    return json_response(req, nlohmann::json{{"error", "not found or not tracked"}}.dump());
                }

                return json_response(req, nlohmann::json{{"success", true}}.dump());
            } catch (...) {
                return json_response(req, nlohmann::json{{"error", "invalid JSON body"}}.dump());
            }
        });

    // GET /api/unreal/status
    router.http_get("/api/unreal/status",
        [this](const request_handle_t& req, router::route_params_t) {
            auto pids = pm_.List();
            nlohmann::json j;
            j["pipe_connected_at"] = ps_.LastConnectTime();

            if (pids.empty()) {
                j["state"] = "idle";
                j["pid"] = 0;
                j["launched_at"] = 0;
                if (pm_.LastExitCode()) {
                    j["exit_code"] = *pm_.LastExitCode();
                    j["exited_at"] = pm_.LastExitTimeMs();
                } else {
                    j["exit_code"] = nullptr;
                    j["exited_at"] = 0;
                }
            } else {
                j["pid"] = pids[0];
                j["launched_at"] = pm_.GetLaunchTimeMs(pids[0]);

                if (pm_.IsStopping(pids[0])) {
                    j["state"] = "stopping";
                } else if (ps_.LastConnectTime() > 0) {
                    j["state"] = "running";
                } else {
                    j["state"] = "launching";
                }
                j["exit_code"] = nullptr;
                j["exited_at"] = 0;
            }

            return json_response(req, j.dump());
        });

    // GET /api/renderstream/schema
    router.http_get("/api/renderstream/schema",
        [](const request_handle_t& req, router::route_params_t) {
            // Parse project from query string
            const auto& target = req->header().request_target();
            auto qpos = target.find('?');
            if (qpos == std::string::npos) {
                return json_response(req, nlohmann::json{{"error", "'project' query param required"}}.dump());
            }

            std::string query = target.substr(qpos + 1);
            std::string project;
            // Simple query string parsing
            std::istringstream qs(query);
            std::string pair;
            while (std::getline(qs, pair, '&')) {
                auto eq = pair.find('=');
                if (eq != std::string::npos) {
                    std::string key = pair.substr(0, eq);
                    std::string val = pair.substr(eq + 1);
                    if (key == "project") {
                        project = val;
                        break;
                    }
                }
            }

            if (project.empty()) {
                return json_response(req, nlohmann::json{{"error", "'project' query param required"}}.dump());
            }

            // URL-decode the project path (handles %20 etc.)
            project = UrlDecode(project);

            spdlog::info("schema: project={}", project);

            auto schema_path = rs::SchemaPath(project);
            auto schema = rs::LoadSchema(schema_path);
            if (!schema) {
                return json_response(req, nlohmann::json{{"error", "schema not found"}}.dump());
            }

            nlohmann::json j;
            j["channels"] = schema->channels;
            nlohmann::json scenes = nlohmann::json::array();
            for (const auto& s : schema->scenes)
                scenes.push_back(s.name);
            j["scenes"] = scenes;

            return json_response(req, j.dump());
        });

    // POST /api/renderstream/launch
    router.http_post("/api/renderstream/launch",
        [this](const request_handle_t& req, router::route_params_t) {
            try {
                auto j = nlohmann::json::parse(req->body());

                std::string engine_exe = j.value("engine_exe", "");
                std::string project = j.value("project", "");
                std::string map = j.value("map", "");
                std::string node_name = j.value("node_name", "");

                if (engine_exe.empty() || project.empty() || map.empty()) {
                    return json_response(req, nlohmann::json{{"error", "engine_exe, project, map are required"}}.dump());
                }

                // Session guard: only one UE session at a time
                if (auto pids = pm_.List(); !pids.empty()) {
                    return json_response(req, nlohmann::json{
                        {"error", "session already active"},
                        {"pid", pids[0]}
                    }.dump());
                }

                // Atomically set pipe data BEFORE launching UE
                ps_.SetStreamData(nlohmann::json{{"streams", j.value("streams", nlohmann::json::array())}}.dump());
                spdlog::info("launch: pipe data set");

                // Write ndisplay config
                if (j.contains("ndisplay") && !j["ndisplay"].is_null()) {
                    wchar_t local_appdata[MAX_PATH]{};
                    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH) == 0)
                        wcscpy_s(local_appdata, L"C:\\Users\\Default\\AppData\\Local");
                    auto tmp = std::filesystem::path(local_appdata) / L"rs-agent";
                    std::filesystem::create_directories(tmp);
                    auto ndisplay_path = tmp / rs::utils::utf8_to_wstring(node_name + ".ndisplay");
                    std::ofstream of(ndisplay_path);
                    of << j["ndisplay"].dump(1, '\t');
                    spdlog::info("launch: wrote ndisplay config to {}", ndisplay_path.string());
                }

                // Build UE command line arguments
                std::wstring engine_exe_w = rs::utils::utf8_to_wstring(engine_exe);
                std::wstring project_w = rs::utils::utf8_to_wstring(project);

                wchar_t local_appdata[MAX_PATH]{};
                if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH) == 0)
                    wcscpy_s(local_appdata, L"C:\\Users\\Default\\AppData\\Local");
                auto ndisplay_path = std::filesystem::path(local_appdata) / L"rs-agent" /
                    rs::utils::utf8_to_wstring(node_name + ".ndisplay");

                std::wstring args;
                args += L"\"" + project_w + L"\"";
                args += L" " + rs::utils::utf8_to_wstring(map);
                args += L" -game -log -dc_cluster"
                        L" -NOSCREENMESSAGES -nohmd -dc_dev_mono -forceres";
                args += L" -ExecCmds=\"r.SceneColorFormat 2\"";
                args += L" -dc_node=" + rs::utils::utf8_to_wstring(node_name);
                args += L" -dc_cfg=\"" + ndisplay_path.wstring() + L"\"";
                args += L" -ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/RenderStream.RenderStreamViewportClient";

                DWORD pid = pm_.Launch(engine_exe_w, args);

                if (pid != 0) {
                    spdlog::info("UE launched: pid={}", pid);
                    return json_response(req, nlohmann::json{{"pid", pid}}.dump());
                } else {
                    ps_.SetStreamData("{\"streams\":[]}");  // roll back
                    return json_response(req, nlohmann::json{{"error", "failed to start UE"}}.dump());
                }
            } catch (const nlohmann::json::parse_error&) {
                return json_response(req, nlohmann::json{{"error", "invalid JSON body"}}.dump());
            }
        });
}
