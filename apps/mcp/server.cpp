#include "mcp/server.hpp"

#include <algorithm>
#include <chrono>

#include "mcp/catalog.hpp"
#include "mcp/tools.hpp"
#include "pixel3d/common/version.hpp"
#include "pixel3d/io/json_safe.hpp"

namespace pixel3d::mcp {

using nlohmann::json;

namespace {

struct RpcError {
  int code;
  std::string message;
  json data;
};

bool valid_id(const json& id) { return id.is_string() || id.is_number_integer(); }

std::string id_key(const json& id) { return to_line(id); }

json op_result_schema() {
  return {{"type", "object"},
          {"properties",
           {{"success", {{"type", "boolean"}}},
            {"status", {{"type", "string"}, {"enum", {"PASS", "NEEDS_REVIEW", "FAIL", "NOT_VALIDATED"}}}},
            {"errors", {{"type", "array"}}},
            {"warnings", {{"type", "array"}}},
            {"info", {{"type", "array"}}},
            {"changed", {{"type", "array"}}},
            {"changed_ids", {{"type", "object"}}},
            {"revision", {{"type", "integer"}}},
            {"stats", {{"type", "object"}}},
            {"metrics", json::object()},
            {"validation", json::object()},
            {"data", json::object()}}},
          {"required", {"success", "status", "errors", "warnings", "revision"}}};
}

}  // namespace

json rpc_error(const json& id, int code, const std::string& message, const json& data) {
  json e{{"code", code}, {"message", message}};
  if (!data.is_null()) e["data"] = data;
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", e}};
}

std::string to_line(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

Server::Server(ServerOptions opt, Send send) : opt_(std::move(opt)), send_(std::move(send)) {}

void Server::log(const char* level, const std::string& text) const {
  if (opt_.log) opt_.log(level, text);
}

void Server::send_json(const json& j) { send_(to_line(j)); }

std::optional<json> Server::cancellation_target(const std::string& line) {
  if (line.find("notifications/cancelled") == std::string::npos) return std::nullopt;
  const auto j = parse_json_untrusted(line, kMaxMessageBytes);
  if (!j || !j->is_object() || j->value("method", "") != "notifications/cancelled") return std::nullopt;
  const json params = j->value("params", json::object());
  if (!params.is_object() || !params.contains("requestId") || !valid_id(params["requestId"])) return std::nullopt;
  return params["requestId"];
}

void Server::cancel(const json& id) {
  std::lock_guard lock(mu_);
  const std::string key = id_key(id);
  if (running_ && *running_ == key) running_stop_.request_stop();
  // A cancellation that arrives after the response is ignored (ids may be reused later).
  if (std::find(done_.begin(), done_.end(), key) != done_.end()) return;
  if (cancelled_.size() >= 4096) cancelled_.clear();
  cancelled_.insert(key);
}

void Server::handle_line(const std::string& line) {
  if (line.find_first_not_of(" \t\r\n") == std::string::npos) return;
  auto parsed = parse_json_untrusted(line, kMaxMessageBytes);
  if (!parsed) {
    send_json(rpc_error(nullptr, rpc::kParseError, "Parse error: " + parsed.error().text));
    return;
  }
  const json& msg = *parsed;
  if (msg.is_array()) {
    if (msg.empty()) {
      send_json(rpc_error(nullptr, rpc::kInvalidRequest, "Invalid Request: empty batch"));
      return;
    }
    json responses = json::array();
    for (const json& m : msg)
      if (auto r = handle_message(m)) responses.push_back(std::move(*r));
    if (!responses.empty()) send_json(responses);
    return;
  }
  if (auto r = handle_message(msg)) send_json(*r);
}

std::optional<json> Server::handle_message(const json& msg) {
  if (!msg.is_object() || msg.value("jsonrpc", json()) != "2.0")
    return rpc_error(msg.is_object() && msg.contains("id") && valid_id(msg["id"]) ? msg["id"] : json(),
                     rpc::kInvalidRequest, "Invalid Request: expected a JSON-RPC 2.0 object");
  if (!msg.contains("method")) return std::nullopt;  // a response from the client: we never send requests
  if (!msg["method"].is_string()) return rpc_error(msg.value("id", json()), rpc::kInvalidRequest, "Invalid Request: method");
  const std::string method = msg["method"].get<std::string>();
  const json params = msg.value("params", json::object());
  if (!msg.contains("id")) {  // notification: never answered
    if (method == "notifications/cancelled" && params.is_object() && params.contains("requestId"))
      cancel(params["requestId"]);
    else if (method == "notifications/initialized")
      log("info", "client initialized");
    return std::nullopt;
  }
  const json id = msg["id"];
  if (!valid_id(id)) return rpc_error(nullptr, rpc::kInvalidRequest, "Invalid Request: id must be a string or integer");
  if (!params.is_object()) return rpc_error(id, rpc::kInvalidParams, "Invalid params: params must be an object");
  {
    std::lock_guard lock(mu_);
    if (cancelled_.erase(id_key(id))) return std::nullopt;  // cancelled before it started: no response
  }
  const auto t0 = std::chrono::steady_clock::now();
  json response;
  try {
    response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", handle_request(method, params, id)}};
  } catch (const RpcError& e) {
    response = rpc_error(id, e.code, e.message, e.data);
  } catch (const std::exception& e) {
    response = rpc_error(id, rpc::kInternalError, std::string("Internal error: ") + e.what());
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  log(response.contains("error") ? "warning" : "info",
      method + (method == "tools/call" ? " " + params.value("name", std::string("?")) : std::string()) + " -> " +
          (response.contains("error") ? "error " + std::to_string(response["error"]["code"].get<int>()) : "ok") + " (" +
          std::to_string(ms) + " ms)");
  {
    std::lock_guard lock(mu_);
    done_.push_back(id_key(id));
    if (done_.size() > 1024) done_.pop_front();
    if (cancelled_.erase(id_key(id))) return std::nullopt;  // cancelled while running
  }
  return response;
}

json Server::handle_request(const std::string& method, const json& params, const json& id) {
  if (method == "ping") return json::object();
  if (method == "initialize") return initialize(params);
  if (!initialized_)
    throw RpcError{rpc::kInvalidRequest, "Invalid Request: the server is not initialized (send initialize first)", nullptr};
  if (method == "tools/list") return tools_list();
  if (method == "tools/call") return call_tool(params, id);
  if (method == "resources/list") return {{"resources", resource_list()}};
  if (method == "resources/templates/list") return {{"resourceTemplates", resource_templates()}};
  if (method == "resources/read") {
    if (!params.contains("uri") || !params["uri"].is_string())
      throw RpcError{rpc::kInvalidParams, "Invalid params: uri is required", nullptr};
    const std::string uri = params["uri"].get<std::string>();
    auto contents = read_resource(session_, uri);
    if (!contents) throw RpcError{rpc::kResourceNotFound, "Resource not found", {{"uri", uri}}};
    return {{"contents", *contents}};
  }
  if (method == "prompts/list") return {{"prompts", prompt_list()}};
  if (method == "prompts/get") {
    if (!params.contains("name") || !params["name"].is_string())
      throw RpcError{rpc::kInvalidParams, "Invalid params: name is required", nullptr};
    auto p = get_prompt(params["name"].get<std::string>(), params.value("arguments", json::object()));
    if (!p) throw RpcError{rpc::kInvalidParams, "Invalid params: unknown prompt", {{"name", params["name"]}}};
    return *p;
  }
  if (method == "logging/setLevel") return json::object();
  throw RpcError{rpc::kMethodNotFound, "Method not found: " + method, nullptr};
}

json Server::initialize(const json& params) {
  const std::string requested = params.value("protocolVersion", std::string());
  protocol_ = kSupportedProtocols[0];
  for (const char* v : kSupportedProtocols)
    if (requested == v) protocol_ = v;
  initialized_ = true;
  if (params.contains("clientInfo") && params["clientInfo"].is_object())
    log("info", "initialize from " + params["clientInfo"].value("name", std::string("?")) + " " +
                    params["clientInfo"].value("version", std::string()) + ", protocol " + protocol_);
  json server{{"name", "pixel3d-mcp"}, {"version", std::string(version())}};
  if (at_least("2025-06-18")) server["title"] = "Pixel3D";
  json result{{"protocolVersion", protocol_},
              {"capabilities",
               {{"tools", {{"listChanged", false}}},
                {"resources", {{"subscribe", false}, {"listChanged", false}}},
                {"prompts", {{"listChanged", false}}},
                {"logging", json::object()}}},
              {"serverInfo", server}};
  if (at_least("2025-03-26")) result["instructions"] = server_instructions();
  return result;
}

json Server::tools_list() const {
  json tools = json::array();
  for (const ToolDef& t : tool_catalog()) {
    json j{{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}};
    if (at_least("2025-06-18")) {
      j["title"] = t.title;
      j["outputSchema"] = op_result_schema();
    }
    if (at_least("2025-03-26"))
      j["annotations"] = {{"title", t.title},
                          {"readOnlyHint", t.read_only},
                          {"destructiveHint", t.destructive},
                          {"idempotentHint", t.idempotent},
                          {"openWorldHint", false}};
    tools.push_back(std::move(j));
  }
  return {{"tools", tools}};
}

json Server::call_tool(const json& params, const json& id) {
  if (!params.contains("name") || !params["name"].is_string())
    throw RpcError{rpc::kInvalidParams, "Invalid params: tool name is required", nullptr};
  const std::string name = params["name"].get<std::string>();
  const ToolDef* tool = find_tool(name);
  if (!tool) throw RpcError{rpc::kInvalidParams, "Unknown tool: " + name, {{"name", name}}};

  // Cancellation and progress for this request.
  std::stop_source stop;
  {
    std::lock_guard lock(mu_);
    running_ = id_key(id);
    running_stop_ = stop;
  }
  JobContext job;
  job.stop = stop.get_token();
  const json meta = params.value("_meta", json::object());
  if (meta.is_object() && meta.contains("progressToken")) {
    const json token = meta["progressToken"];
    auto last = std::make_shared<float>(-1.0f);
    job.progress = [this, token, last](float f, std::string_view stage) {
      if (f <= *last) return;  // progress must increase
      *last = f;
      send_json({{"jsonrpc", "2.0"},
                 {"method", "notifications/progress"},
                 {"params", {{"progressToken", token}, {"progress", f}, {"total", 1.0}, {"message", std::string(stage)}}}});
    };
  }

  ToolOutput out;
  try {
    const Args args(params.value("arguments", json::object()));
    ToolContext ctx{session_, opt_.paths, job};
    out = tool->run(ctx, args);
  } catch (const ArgError& e) {
    out.result = session_.current_status();
    out.result.success = false;
    out.result.errors = {Message{"input.invalid", e.what(), Severity::Error, {{"field", e.field}, {"tool", name}}}};
  } catch (const std::exception& e) {
    out.result = session_.current_status();
    out.result.success = false;
    out.result.errors = {Message{"internal.exception", e.what(), Severity::Error, {{"tool", name}}}};
  }
  {
    std::lock_guard lock(mu_);
    running_.reset();
  }

  // Images inline, within a size budget; anything beyond it is reported.
  json images = json::array();
  size_t budget = kMaxInlineImageBytes;
  std::vector<std::string> skipped;
  for (const NamedImage& img : out.images) {
    const std::vector<uint8_t> png = encode_png(img.image);
    std::string b64 = base64(std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
    if (b64.size() > budget) {
      skipped.push_back(img.name);
      continue;
    }
    budget -= b64.size();
    images.push_back({{"type", "image"}, {"data", std::move(b64)}, {"mimeType", "image/png"}});
  }
  if (!skipped.empty())
    out.result.add(Message{"render.not_inlined", "some images exceed the inline size budget; use output_dir",
                           Severity::Warning, {{"images", skipped}}});
  json structured = to_json(out.result);
  if (!out.images.empty()) {
    json names = json::array();
    for (const NamedImage& img : out.images)
      names.push_back({{"name", img.name}, {"width", img.image.width()}, {"height", img.image.height()}});
    structured["images"] = names;  // in the order of the image content blocks (minus skipped)
  }
  json content = json::array({{{"type", "text"}, {"text", to_line(structured)}}});
  for (json& im : images) content.push_back(std::move(im));
  json result{{"content", content}, {"isError", !out.result.success}};
  if (at_least("2025-06-18")) result["structuredContent"] = structured;
  return result;
}

}  // namespace pixel3d::mcp
