// A language-model brain behind an OpenAI-compatible chat endpoint (llama.cpp `llama-server --jinja`, LM Studio):
// the conversation, the tool schemas and the attached pictures (downscaled PNG, base64) go in; text and tool calls
// come out. Studied in docs/research/agent-chat-brain.md; evaluated in docs/agent/chat-eval/. The model plans and
// explains; results still come only from the tools (the agent's files).
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <sstream>

#include "chat.hpp"
#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/image/image.hpp"

namespace p3dchat {

namespace {

std::string base64(const std::vector<uint8_t>& in) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    const uint32_t n = (uint32_t(in[i]) << 16) | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0) | (i + 2 < in.size() ? in[i + 2] : 0);
    out += k[(n >> 18) & 63];
    out += k[(n >> 12) & 63];
    out += i + 1 < in.size() ? k[(n >> 6) & 63] : '=';
    out += i + 2 < in.size() ? k[n & 63] : '=';
  }
  return out;
}

// The picture as a PNG of at most `side` pixels (box filter), on white where it is transparent.
std::optional<std::string> picture_data_url(const fs::path& file, int side) {
  auto bytes = pixel3d::read_file(file, 256u << 20);
  if (!bytes) return std::nullopt;
  auto img = pixel3d::decode_image(*bytes, pixel3d::to_utf8(file.filename()));
  if (!img) return std::nullopt;
  const int W = img->width(), H = img->height();
  const double s = std::min(1.0, double(side) / std::max(W, H));
  const int w = std::max(1, int(W * s)), h = std::max(1, int(H * s));
  pixel3d::ImageRGBA out(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int x0 = int(x / s), x1 = std::max(x0 + 1, int((x + 1) / s)), y0 = int(y / s), y1 = std::max(y0 + 1, int((y + 1) / s));
      double r = 0, g = 0, b = 0, n = 0;
      for (int yy = y0; yy < std::min(H, y1); ++yy)
        for (int xx = x0; xx < std::min(W, x1); ++xx) {
          const auto& p = img->at(xx, yy);
          const double a = p.a / 255.0;
          r += p.r * a + 255 * (1 - a), g += p.g * a + 255 * (1 - a), b += p.b * a + 255 * (1 - a), n += 1;
        }
      out.at(x, y) = {uint8_t(r / std::max(1.0, n)), uint8_t(g / std::max(1.0, n)), uint8_t(b / std::max(1.0, n)), 255};
    }
  return "data:image/png;base64," + base64(pixel3d::encode_png(out));
}

std::string post(const std::wstring& host, int port, const std::wstring& path, const std::string& body, int timeout_ms) {
  HINTERNET s = WinHttpOpen(L"3re-chat/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) throw std::runtime_error("WinHttpOpen failed");
  WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_PORT(port), 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
  std::string out;
  bool ok = r && WinHttpSendRequest(r, L"Content-Type: application/json\r\n", DWORD(-1), (LPVOID)body.data(), DWORD(body.size()), DWORD(body.size()), 0) &&
            WinHttpReceiveResponse(r, nullptr);
  DWORD status = 0, len = sizeof status;
  if (ok) WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
  for (DWORD avail = 0; ok && WinHttpQueryDataAvailable(r, &avail) && avail > 0;) {
    std::string chunk(avail, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(r, chunk.data(), avail, &read)) break;
    out.append(chunk.data(), read);
  }
  const DWORD err = GetLastError();
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  if (!ok) throw std::runtime_error("the model server did not answer (Windows error " + std::to_string(err) + ")");
  if (status != 200) throw std::runtime_error("the model server answered HTTP " + std::to_string(status) + ": " + out.substr(0, 300));
  return out;
}

const char* kSystem =
    "You are the brain of 3re, a game-ready 3D asset agent on the user's laptop. You talk with a person (Arabic or English: "
    "answer in their language) and operate the agent only through the tools. Rules:\n"
    "1. To start work you need: at least one attached picture (or a GLB), whether it is a character or a prop, a platform "
    "(mobile, pc, hero) or a triangle budget, and the real size in metres. If one is missing, ask ONE short question for the "
    "first missing one and call no tool.\n"
    "2. When everything is there: call propose_labels with all picture paths (give 'labels' {path: side} for the sides you "
    "can see: front, back, left, right, front_left, front_right, back_left, back_right; the 3D shape comes from the front), "
    "then make_spec (picture = the front from propose_labels, views = its views; requested_style if a style was named), "
    "then validate_spec, then run_agent.\n"
    "3. Never invent results: PASS/REVIEW/FAIL and numbers come only from tool results. Never answer the agent's question for "
    "the person: call answer_decision only with the option the person chose in words (a picture option needs an attached picture).\n"
    "4. 3re cannot rig or animate generated characters and does not apply styles (a style is recorded only); say so plainly.\n"
    "5. 'status' questions: call run_status for the current run. A person pointing at a problem in a finished model: "
    "region_views, then regional_fix with their mark.\n"
    "Keep replies short.";

}  // namespace

HttpProvider::HttpProvider(std::string host, int port, std::string model, int picture_side, int timeout_seconds)
    : host_(std::move(host)), port_(port), model_(std::move(model)), side_(picture_side), timeout_(timeout_seconds) {}

Reply HttpProvider::next(const std::vector<Message>& c, const json& tools) {
  json msgs = json::array({{{"role", "system"}, {"content", kSystem}}});
  // The current run, so the model can name it in run_status / answer_decision.
  std::string run;
  for (const auto& m : c) {
    if (m.role == "user") {
      std::string text = m.text;
      json parts = json::array();
      for (const auto& a : m.attachments) {
        text += "\n[attached " + a.kind + ": " + pixel3d::to_utf8(a.path) + "]";
        if (a.kind == "picture")
          if (auto url = picture_data_url(a.path, side_))
            parts.push_back(json::object({{"type", "image_url"}, {"image_url", json::object({{"url", *url}})}}));
      }
      if (parts.empty()) msgs.push_back({{"role", "user"}, {"content", text}});
      else {
        parts.insert(parts.begin(), json::object({{"type", "text"}, {"text", text}}));  // explicit: a braced list would insert two items
        msgs.push_back({{"role", "user"}, {"content", parts}});
      }
    } else if (m.role == "assistant") {
      json a = {{"role", "assistant"}, {"content", m.text}};
      if (!m.calls.empty()) {
        json calls = json::array();
        for (const auto& t : m.calls) calls.push_back({{"id", t.id}, {"type", "function"}, {"function", {{"name", t.name}, {"arguments", t.args.dump()}}}});
        a["tool_calls"] = calls;
      }
      msgs.push_back(a);
    } else if (m.role == "tool") {
      if (m.tool == "run_agent" && m.result.value("ok", false)) run = m.result.value("run", std::string());
      std::string r = m.result.dump();
      if (r.size() > 3000) r = r.substr(0, 3000) + "…";
      msgs.push_back({{"role", "tool"}, {"tool_call_id", m.call_id}, {"content", r}});
    } else if (m.role == "event") {
      std::string r = m.result.dump();
      if (r.size() > 3000) r = r.substr(0, 3000) + "…";
      msgs.push_back({{"role", "user"}, {"content", "[agent run update, measured by the agent — tell the person what it says]\n" + r}});
    }
  }
  if (!run.empty()) msgs[0]["content"] = std::string(kSystem) + "\nCurrent agent run folder: " + run;
  json fns = json::array();
  for (const auto& t : tools)
    fns.push_back({{"type", "function"}, {"function", {{"name", t["name"]}, {"description", t["description"]}, {"parameters", t["input_schema"]}}}});
  const json body = {{"model", model_}, {"messages", msgs}, {"tools", fns}, {"temperature", 0}, {"seed", 42}, {"max_tokens", 600}};
  const auto t0 = GetTickCount64();
  const json res = json::parse(post(std::wstring(host_.begin(), host_.end()), port_, L"/v1/chat/completions", body.dump(-1, ' ', false, json::error_handler_t::replace),
                                    timeout_ * 1000));
  last_seconds_ = double(GetTickCount64() - t0) / 1000.0;
  last_usage_ = res.value("usage", json::object());
  Reply out;
  const json msg = res.at("choices").at(0).at("message");
  if (msg.contains("content") && msg["content"].is_string()) out.text = msg["content"].get<std::string>();
  for (const auto& tc : msg.value("tool_calls", json::array())) {
    ToolCall call;
    call.id = tc.value("id", std::string());
    call.name = tc["function"].value("name", std::string());
    const json args = tc["function"].value("arguments", json());
    try {
      call.args = args.is_string() ? json::parse(args.get<std::string>()) : args;
    } catch (...) {
      call.args = json::object();  // unparsable arguments: the tool refuses them
    }
    out.calls.push_back(call);
  }
  return out;
}

}  // namespace p3dchat
