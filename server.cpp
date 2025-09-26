// server.cpp
// Simple multi-threaded TCP server (echo server) with a thread-pool.
// Build: g++ -std=c++17 -O2 -pthread server.cpp -o tcpserver
// Run:  ./tcpserver 8080 4
//
// Author: ChatGPT — example for Unix/Linux

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_running{true};

void handle_sigint(int) {
    g_running = false;
    // Note: main will close the listening socket to unblock accept()
}

// Utility: set socket to non-blocking (optional; we use blocking sockets here)
bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return false;
    return true;
}

// ThreadPool that accepts integer client sockets (file descriptors)
class ThreadPool {
public:
    ThreadPool(size_t numThreads) : stopped(false) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~ThreadPool() {
        stop();
    }

    // Enqueue a client socket fd
    void enqueue(int clientFd) {
        {
            std::unique_lock<std::mutex> lk(mtx);
            tasks.push(clientFd);
        }
        cv.notify_one();
    }

    // Stop the pool and join threads
    void stop() {
        {
            std::unique_lock<std::mutex> lk(mtx);
            if (stopped) return;
            stopped = true;
        }
        cv.notify_all();
        for (auto &t : workers) {
            if (t.joinable()) t.join();
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<int> tasks;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopped;

    void workerLoop() {
        while (true) {
            int clientFd = -1;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [this] { return stopped || !tasks.empty(); });
                if (stopped && tasks.empty()) return;
                clientFd = tasks.front();
                tasks.pop();
            }
            if (clientFd >= 0) {
                serveClient(clientFd);
            }
        }
    }

    // Basic echo logic: read from client and echo back until client closes.
    void serveClient(int fd) {
        constexpr size_t BUF_SIZE = 8192;
        char buffer[BUF_SIZE];

        // Optional: set a timeout on the socket (not required)
        // struct timeval tv{10, 0}; // 10 seconds
        // setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

        while (true) {
            ssize_t n = recv(fd, buffer, BUF_SIZE, 0);
            if (n > 0) {
                // Echo back the same bytes
                ssize_t sent = 0;
                while (sent < n) {
                    ssize_t s = send(fd, buffer + sent, n - sent, 0);
                    if (s < 0) {
                        if (errno == EINTR) continue;
                        perror("send");
                        close(fd);
                        return;
                    }
                    sent += s;
                }
            } else if (n == 0) {
                // client closed connection
                break;
            } else { // n < 0
                if (errno == EINTR) continue;
                // For EAGAIN/EWOULDBLOCK we could continue; but using blocking sockets normally
                // indicates an error — break and close.
                // If the server wants to be tolerant, handle EAGAIN specially.
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // No data right now — continue waiting (not expected with blocking sockets)
                    continue;
                }
                // other errors
                // perror("recv");
                break;
            }
        }
        close(fd);
    }
};


// Create, bind, listen, accept loop.
// Usage: ./tcpserver <port> <num_worker_threads>
int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <port> [num_threads=4]\n";
        return 1;
    }

    int port = std::atoi(argv[1]);
    int numThreads = 4;
    if (argc >= 3) numThreads = std::max(1, std::atoi(argv[2]));

    signal(SIGINT, handle_sigint);

    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        perror("socket");
        return 1;
    }

    // allow reuse of address quickly after restart
    int opt = 1;
    if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        // not fatal
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenFd);
        return 1;
    }

    if (listen(listenFd, SOMAXCONN) < 0) {
        perror("listen");
        close(listenFd);
        return 1;
    }

    std::cout << "Server listening on port " << port << " with " << numThreads << " worker threads.\n";
    ThreadPool pool(static_cast<size_t>(numThreads));

    while (g_running) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(listenFd, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientFd < 0) {
            if (errno == EINTR) {
                // interrupted by signal (maybe SIGINT) — break if shutting down
                continue;
            }
            // If g_running is false, break
            if (!g_running) break;
            perror("accept");
            // small sleep to avoid busy loop on repeated errors
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char addrbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, addrbuf, sizeof(addrbuf));
        std::cout << "Accepted connection from " << addrbuf << ":" << ntohs(clientAddr.sin_port) << ", fd=" << clientFd << "\n";

        // Hand off to pool
        pool.enqueue(clientFd);
    }

    // Shutdown sequence
    std::cout << "\nShutting down server...\n";
    // Close listening socket so accept is unblocked if still waiting
    close(listenFd);

    // Stop thread pool (it will finish existing clients)
    pool.stop();

    std::cout << "Server stopped.\n";
    return 0;
}
