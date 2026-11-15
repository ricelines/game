#include "net.hpp"

#include <App.h>

#include <algorithm>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace amber::net {
namespace {
struct SocketData {
  SessionId session = 0;
  bool binary = false;
  bool stale = false;
};
using Socket = uWS::WebSocket<false, true, SocketData>;
bool below(const std::filesystem::path &root, const std::filesystem::path &file) {
  auto r = root.begin(), f = file.begin();
  for (; r != root.end(); ++r, ++f)
    if (f == file.end() || *r != *f)
      return false;
  return true;
}
std::string_view mime(const std::filesystem::path &path) {
  const auto ext = path.extension().string();
  if (ext == ".html")
    return "text/html; charset=utf-8";
  if (ext == ".js")
    return "text/javascript; charset=utf-8";
  if (ext == ".wasm")
    return "application/wasm";
  if (ext == ".json")
    return "application/json";
  if (ext == ".glb")
    return "model/gltf-binary";
  if (ext == ".png")
    return "image/png";
  if (ext == ".svg")
    return "image/svg+xml";
  if (ext == ".ico")
    return "image/x-icon";
  return "application/octet-stream";
}
void file_response(uWS::HttpResponse<false> *response, uWS::HttpRequest *request,
                   const std::filesystem::path &root, std::string_view relative, bool immutable) {
  std::error_code error;
  auto base = std::filesystem::weakly_canonical(root, error);
  if (error || root.empty() || relative.find('%') != relative.npos) {
    response->writeStatus("404 Not Found")->end();
    return;
  }
  auto path = std::filesystem::weakly_canonical(base / relative, error);
  if (error || !below(base, path) || !std::filesystem::is_regular_file(path, error)) {
    response->writeStatus("404 Not Found")->end();
    return;
  }
  auto source = path;
  bool compressed = false;
  if (request->getHeader("accept-encoding").find("gzip") != std::string_view::npos) {
    auto gzip = std::filesystem::path(path.string() + ".gz");
    gzip = std::filesystem::weakly_canonical(gzip, error);
    if (!error && below(base, gzip) && std::filesystem::is_regular_file(gzip, error)) {
      source = std::move(gzip);
      compressed = true;
    }
  }
  std::ifstream file(source, std::ios::binary | std::ios::ate);
  if (!file) {
    response->writeStatus("404 Not Found")->end();
    return;
  }
  const auto size = file.tellg();
  auto bytes = std::make_shared<std::string>(std::size_t(size), '\0');
  file.seekg(0);
  file.read(bytes->data(), size);
  if (!file) {
    response->writeStatus("500 Internal Server Error")->end();
    return;
  }
  response->writeHeader("Content-Type", mime(path))
      ->writeHeader("Cache-Control", immutable ? "public,max-age=31536000,immutable" : "no-cache")
      ->writeHeader("Vary", "Accept-Encoding");
  if (compressed)
    response->writeHeader("Content-Encoding", "gzip");
  auto [accepted, done] = response->tryEnd(*bytes, bytes->size());
  (void)accepted;
  if (!done) {
    response->onAborted([bytes] {});
    response->onWritable([response, bytes](std::uintmax_t offset) {
      auto [ok, complete] =
          response->tryEnd(std::string_view(*bytes).substr(offset), bytes->size());
      (void)complete;
      return ok;
    });
  }
}
} // namespace
struct Server::Data {
  ServerOptions options;
  Replicator replication;
  std::mutex lifecycle, commands, inputs_mutex;
  std::thread worker;
  uWS::Loop *loop = nullptr;
  uWS::App *app = nullptr;
  std::vector<Input> inputs;
  std::map<SessionId, Socket *> sockets;
  SessionId next_session = 1;
  explicit Data(ServerOptions o) : options(std::move(o)), replication(options.replication) {}
  template <class T> void input(T message) {
    std::lock_guard lock(inputs_mutex);
    inputs.emplace_back(std::move(message));
  }
  template <class Function> void defer(Function function) {
    std::lock_guard lock(commands);
    if (loop)
      loop->defer(std::move(function));
  }
  void close_slow_viewer(Socket *socket) {
    input(SendOverflow{socket->getUserData()->session, socket->getBufferedAmount(), true});
    socket->end(1008, "slow_viewer");
  }
  bool check_send(Socket *socket) {
    const auto buffered = socket->getBufferedAmount();
    auto &data = *socket->getUserData();
    const bool stale = buffered > options.replication.send_soft;
    if (stale && !data.stale)
      input(SendOverflow{data.session, buffered, false});
    data.stale = stale;
    replication.set_writable(data.session, !stale);
    if (buffered > options.replication.send_hard) {
      close_slow_viewer(socket);
      return false;
    }
    return true;
  }
  void transmit(Publication publication) {
    for (auto it = sockets.begin(); it != sockets.end();) {
      auto *socket = (it++)->second;
      check_send(socket);
    }
    std::vector<Delivery> deliveries;
    try {
      deliveries = replication.advance(std::move(publication));
    } catch (const std::exception &error) {
      input(Fault{error.what()});
      for (auto it = sockets.begin(); it != sockets.end();) {
        auto *socket = (it++)->second;
        if (socket->getUserData()->binary)
          socket->end(1011, "replication_capacity_or_data_error");
      }
      return;
    }
    for (const auto &delivery : deliveries) {
      auto bytes = std::string_view(reinterpret_cast<const char *>(delivery.bytes->data()),
                                    delivery.bytes->size());
      for (auto session : delivery.recipients) {
        auto found = sockets.find(session);
        if (found == sockets.end())
          continue;
        auto *socket = found->second;
        if (!check_send(socket) || socket->getUserData()->stale)
          continue;
        auto status = socket->send(bytes, uWS::BINARY);
        if (status == Socket::DROPPED) {
          close_slow_viewer(socket);
        } else {
          // BACKPRESSURE means uWS retained the unsent suffix. Throttle only
          // above SEND_SOFT, not merely because a write needed buffering.
          replication.sent(session, delivery);
          check_send(socket);
        }
      }
    }
  }
};
Server::Server(ServerOptions options) : data(std::make_unique<Data>(std::move(options))) {}
Server::~Server() {
  stop();
}
std::string Server::start() {
  auto &n = *data;
  std::lock_guard lock(n.lifecycle);
  if (n.worker.joinable())
    return "transport already started";
  std::promise<std::string> ready;
  auto result = ready.get_future();
  n.worker = std::thread([&n, &ready] {
    uWS::App application;
    {
      std::lock_guard command_lock(n.commands);
      n.loop = uWS::Loop::get();
      n.app = &application;
    }
    uWS::App::WebSocketBehavior<SocketData> behavior;
    behavior.compression = uWS::DISABLED;
    behavior.maxPayloadLength = n.options.max_control_bytes;
    // Our hard cap closes explicitly; disabling the library drop cap lets its
    // close frame carry the reason even after the buffer crosses that cap.
    behavior.maxBackpressure = 0;
    behavior.closeOnBackpressureLimit = false;
    behavior.open = [&n](auto *socket) {
      const auto session = n.next_session++;
      socket->getUserData()->session = session;
      n.sockets.emplace(session, socket);
      n.input(Connected{session});
    };
    behavior.message = [&n](auto *socket, std::string_view message, uWS::OpCode opcode) {
      if (opcode != uWS::TEXT) {
        socket->end(1003, "controls_must_be_text");
        return;
      }
      n.input(Control{socket->getUserData()->session, std::string(message)});
    };
    behavior.close = [&n](auto *socket, int code, std::string_view reason) {
      const auto session = socket->getUserData()->session;
      n.replication.remove_viewer(session);
      n.sockets.erase(session);
      n.input(Disconnected{session, std::uint16_t(code), std::string(reason)});
    };
    application.ws<SocketData>("/ws", std::move(behavior));
    application.get("/*", [&n](auto *response, auto *request) {
      auto url = request->getUrl();
      if (url.starts_with("/assets/"))
        file_response(response, request, n.options.assets_root, url.substr(8), true);
      else
        file_response(response, request, n.options.client_root,
                      url == "/" ? "index.html" : url.substr(1), false);
    });
    application.listen(n.options.bind_address, n.options.port, [&ready](auto *listener) {
      ready.set_value(listener ? std::string{} : "could not listen on configured transport port");
    });
    application.run();
    {
      std::lock_guard command_lock(n.commands);
      n.loop = nullptr;
      n.app = nullptr;
    }
  });
  auto error = result.get();
  if (!error.empty())
    n.worker.join();
  return error;
}
void Server::stop() {
  auto &n = *data;
  std::lock_guard lock(n.lifecycle);
  if (!n.worker.joinable())
    return;
  n.defer([&n] { n.app->close(); });
  n.worker.join();
}
void Server::drain(std::vector<Input> &out) {
  out.clear();
  std::lock_guard lock(data->inputs_mutex);
  out.swap(data->inputs);
}
void Server::send(SessionId session, std::string text) {
  auto *n = data.get();
  n->defer([n, session, text = std::move(text)] {
    auto found = n->sockets.find(session);
    if (found == n->sockets.end())
      return;
    auto *socket = found->second;
    if (!n->check_send(socket))
      return;
    if (socket->send(text, uWS::TEXT) == Socket::DROPPED)
      n->close_slow_viewer(socket);
    else
      n->check_send(socket);
  });
}
void Server::close(SessionId session, std::uint16_t code, std::string reason) {
  auto *n = data.get();
  n->defer([n, session, code, reason = std::move(reason)] {
    auto found = n->sockets.find(session);
    if (found != n->sockets.end())
      found->second->end(code, std::string_view(reason).substr(0, 123));
  });
}
void Server::view(SessionId session, Vec3 position) {
  auto *n = data.get();
  n->defer([n, session, position] {
    auto found = n->sockets.find(session);
    if (found == n->sockets.end())
      return;
    try {
      n->replication.set_view(session, position);
      found->second->getUserData()->binary = true;
    } catch (const std::exception &error) {
      n->input(Fault{error.what()});
      found->second->end(1008, "invalid_view_position");
    }
  });
}
void Server::publish(Publication publication) {
  auto *n = data.get();
  n->defer(
      [n, publication = std::move(publication)]() mutable { n->transmit(std::move(publication)); });
}
} // namespace amber::net
