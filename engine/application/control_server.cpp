#include "control_server.h"

#include <engine/core/log/log.h>

#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace me
{

namespace
{
#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

void CloseSocket(NativeSocket socket)
{
    closesocket(socket);
}

void ShutdownSocket(NativeSocket socket)
{
    shutdown(socket, SD_BOTH);
}

std::string SocketError()
{
    return "WSA error " + std::to_string(WSAGetLastError());
}

// Winsock is started once for the process and left running; WSAStartup counts its callers.
void StartSockets()
{
    static const bool started = []()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            throw std::runtime_error("WSAStartup failed");
        }
        return true;
    }();
    (void)started;
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

void CloseSocket(NativeSocket socket)
{
    close(socket);
}

void ShutdownSocket(NativeSocket socket)
{
    shutdown(socket, SHUT_RDWR);
}

std::string SocketError()
{
    return "errno " + std::to_string(errno);
}

void StartSockets()
{
}
#endif

NativeSocket ToNative(std::intptr_t socket)
{
    return static_cast<NativeSocket>(socket);
}

// A line longer than this is not a request: the connection is closed rather than buffered forever.
constexpr size_t kMaxLineBytes = 16u * 1024u * 1024u;
}

struct ControlServer::Connection
{
    uint64_t id = 0;
    NativeSocket socket = kInvalidSocket;
    std::mutex writeMutex;
    std::atomic<bool> open{true};
    // Its read thread has returned and can be joined.
    std::atomic<bool> finished{false};
};

ControlServer::ControlServer(uint16_t port)
{
    StartSockets();
    const NativeSocket listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == kInvalidSocket)
    {
        throw std::runtime_error("Control channel: cannot create a socket (" + SocketError() + ")");
    }
#ifdef _WIN32
    // Another process may not take the port while this one has it.
    BOOL exclusive = TRUE;
    setsockopt(listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(listenSocket, 4) != 0)
    {
        const std::string error = SocketError();
        CloseSocket(listenSocket);
        throw std::runtime_error("Control channel: cannot listen on 127.0.0.1:" + std::to_string(port) + " (" + error + "); is another engine using it?");
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int boundSize = sizeof(bound);
#else
    socklen_t boundSize = sizeof(bound);
#endif
    getsockname(listenSocket, reinterpret_cast<sockaddr*>(&bound), &boundSize);
    m_port = ntohs(bound.sin_port);
    m_listenSocket = static_cast<std::intptr_t>(listenSocket);
    m_acceptThread = std::thread([this]()
                                 {
                                     AcceptLoop();
                                 });
}

ControlServer::~ControlServer()
{
    m_stopping = true;
    // Closing the sockets wakes the threads blocked in accept and recv.
    CloseSocket(ToNative(m_listenSocket));
    if (m_acceptThread.joinable())
    {
        m_acceptThread.join();
    }
    std::vector<std::pair<std::shared_ptr<Connection>, std::thread>> readThreads;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, connection] : m_connections)
        {
            ShutdownSocket(connection->socket);
        }
        readThreads.swap(m_readThreads);
    }
    for (auto& [connection, thread] : readThreads)
    {
        thread.join();
    }
}

void ControlServer::AcceptLoop()
{
    while (!m_stopping)
    {
        const NativeSocket client = accept(ToNative(m_listenSocket), nullptr, nullptr);
        if (client == kInvalidSocket)
        {
            if (m_stopping)
            {
                return;
            }
            continue;
        }
        auto connection = std::make_shared<Connection>();
        connection->socket = client;
        std::lock_guard<std::mutex> lock(m_mutex);
        // A client that connects for every request leaves a finished thread each time.
        for (auto reader = m_readThreads.begin(); reader != m_readThreads.end();)
        {
            if (reader->first->finished)
            {
                reader->second.join();
                reader = m_readThreads.erase(reader);
            }
            else
            {
                ++reader;
            }
        }
        connection->id = m_nextConnection++;
        m_connections.emplace(connection->id, connection);
        m_readThreads.emplace_back(connection, std::thread([this, connection]()
                                                           {
                                                               ReadLoop(connection);
                                                           }));
        spdlog::debug("Control channel: client {} connected", connection->id);
    }
}

void ControlServer::ReadLoop(std::shared_ptr<Connection> connection)
{
    std::string pending;
    char buffer[64 * 1024];
    while (!m_stopping)
    {
        const int received = recv(connection->socket, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0)
        {
            break;
        }
        pending.append(buffer, static_cast<size_t>(received));
        size_t start = 0;
        for (size_t newline = pending.find('\n', start); newline != std::string::npos; newline = pending.find('\n', start))
        {
            std::string line = pending.substr(start, newline - start);
            start = newline + 1;
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_requests.push_back({connection->id, std::move(line)});
        }
        pending.erase(0, start);
        if (pending.size() > kMaxLineBytes)
        {
            LOG_WARN("Control channel: client {} sent a line over {} bytes; closing it", connection->id, kMaxLineBytes);
            break;
        }
    }
    // Out of the map first, so the destructor never shuts down a socket closed here.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.erase(connection->id);
    }
    {
        std::lock_guard<std::mutex> writeLock(connection->writeMutex);
        connection->open = false;
        CloseSocket(connection->socket);
    }
    if (!m_stopping)
    {
        spdlog::debug("Control channel: client {} disconnected", connection->id);
    }
    connection->finished = true;
}

std::vector<ControlServer::Request> ControlServer::TakeRequests()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Request> requests;
    requests.swap(m_requests);
    return requests;
}

bool ControlServer::Send(uint64_t connectionId, const std::string& line)
{
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_connections.find(connectionId);
        if (found == m_connections.end())
        {
            return false;
        }
        connection = found->second;
    }
    std::lock_guard<std::mutex> writeLock(connection->writeMutex);
    if (!connection->open)
    {
        return false;
    }
    std::string text = line;
    text.push_back('\n');
    size_t sent = 0;
    while (sent < text.size())
    {
        const int result = send(connection->socket, text.data() + sent, static_cast<int>(text.size() - sent), 0);
        if (result <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

size_t ControlServer::ConnectionCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connections.size();
}
}
