#pragma once
// MCP server protocol layer (JSON-RPC 2.0, one message per line). Transport-free:
// main.cpp feeds lines from stdin and writes what `send` receives to the
// protocol stdout; tests drive it in-process. One Session per server; requests
// run in order on the caller's thread, cancellation may arrive from another.
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>

#include <nlohmann/json.hpp>

#include "mcp/args.hpp"
#include "pixel3d/engine/session.hpp"

namespace pixel3d::mcp {

inline constexpr const char* kSupportedProtocols[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
inline constexpr size_t kMaxMessageBytes = 64u << 20;
inline constexpr size_t kMaxInlineImageBytes = 16u << 20;  // base64 per response

namespace rpc {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kResourceNotFound = -32002;
}  // namespace rpc

struct ServerOptions {
  PathPolicy paths;
  // level: "debug" | "info" | "warning" | "error"; never written to the protocol stream.
  std::function<void(const char* level, const std::string& text)> log;
};

class Server {
 public:
  using Send = std::function<void(const std::string& line)>;
  Server(ServerOptions opt, Send send);

  // One raw line from the client: a message or a batch. Replies go to `send`.
  void handle_line(const std::string& line);
  // Thread-safe. Stops request `id` if it is running; drops it if it has not started.
  void cancel(const nlohmann::json& id);
  // Reader-thread helper: the request id if `line` is a notifications/cancelled message.
  static std::optional<nlohmann::json> cancellation_target(const std::string& line);

  const std::string& protocol_version() const { return protocol_; }
  Session& session() { return session_; }

 private:
  std::optional<nlohmann::json> handle_message(const nlohmann::json& msg);
  nlohmann::json handle_request(const std::string& method, const nlohmann::json& params, const nlohmann::json& id);
  nlohmann::json initialize(const nlohmann::json& params);
  nlohmann::json tools_list() const;
  nlohmann::json call_tool(const nlohmann::json& params, const nlohmann::json& id);
  bool at_least(const char* version) const { return protocol_ >= version; }
  void log(const char* level, const std::string& text) const;
  void send_json(const nlohmann::json& j);

  ServerOptions opt_;
  Send send_;
  Session session_{Source::Mcp};
  bool initialized_ = false;
  std::string protocol_ = kSupportedProtocols[0];

  std::mutex mu_;                       // guards the cancellation state below
  std::set<std::string> cancelled_;     // ids (dumped) cancelled before they started or while running
  std::deque<std::string> done_;        // recently answered ids
  std::optional<std::string> running_;  // id of the running request
  std::stop_source running_stop_;
};

// JSON-RPC error object as a full response.
nlohmann::json rpc_error(const nlohmann::json& id, int code, const std::string& message,
                         const nlohmann::json& data = nullptr);
// Serialises one message on one line (invalid UTF-8 is replaced, never thrown on).
std::string to_line(const nlohmann::json& j);

}  // namespace pixel3d::mcp
