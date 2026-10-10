#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me
{

// The socket side of the engine's control channel (--control; docs/design/2026-10-10-engine-control-channel-design.md):
// a TCP server on 127.0.0.1 that takes one JSON request a line from any number of local clients. Its
// threads only move text: the frame loop takes the requests (TakeRequests), runs them between frames
// and answers on the same connection (Send). Nothing is reachable from another machine.
class ControlServer
{
  public:
    struct Request
    {
        // Which connection to answer on (Send); a connection that closed drops what is sent to it.
        uint64_t connection = 0;
        std::string line;
    };

    // Listens on 127.0.0.1:port; port 0 takes any free one (Port() tells which). Throws when the port
    // cannot be bound, for instance because another engine already listens on it.
    explicit ControlServer(uint16_t port);
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    uint16_t Port() const
    {
        return m_port;
    }
    // The lines received since the last call, oldest first.
    std::vector<Request> TakeRequests();
    // Writes `line` and a newline to the connection; false when it has closed.
    bool Send(uint64_t connection, const std::string& line);
    size_t ConnectionCount() const;

  private:
    struct Connection;

    void AcceptLoop();
    void ReadLoop(std::shared_ptr<Connection> connection);

    uint16_t m_port = 0;
    std::intptr_t m_listenSocket = -1;
    std::atomic<bool> m_stopping{false};
    std::thread m_acceptThread;

    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<Connection>> m_connections;
    std::vector<std::pair<std::shared_ptr<Connection>, std::thread>> m_readThreads;
    std::vector<Request> m_requests;
    uint64_t m_nextConnection = 1;
};
}
