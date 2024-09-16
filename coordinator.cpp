#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <netdb.h>
#include <cstdarg>
const int PORT = 8080;
const int MAX_RETRIES = 5;
const int RETRY_INTERVAL_MS = 1000;

std::vector<std::string> hosts;
std::string my_hostname;
std::atomic<bool> all_ready(false);
std::mutex ready_mutex;
std::vector<bool> ready_hosts;

void log_debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "DEBUG: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

std::vector<std::string> read_hostfile(const std::string& filename) {
    std::vector<std::string> hosts;
    std::ifstream file(filename);
    std::string line;
    while (std::getline(file, line)) {
        hosts.push_back(line);
    }
    log_debug("Read %zu hosts from hostfile", hosts.size());
    return hosts;
}

void send_message(int sock, const std::string& message, const std::string& dest_hostname) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    log_debug("Resolving hostname: %s", dest_hostname.c_str());
    int status = getaddrinfo(dest_hostname.c_str(), std::to_string(PORT).c_str(), &hints, &res);
    if (status != 0) {
        log_debug("getaddrinfo error: %s", gai_strerror(status));
        return;
    }

    log_debug("Sending message to %s: %s", dest_hostname.c_str(), message.c_str());
    sendto(sock, message.c_str(), message.length(), 0, res->ai_addr, res->ai_addrlen);

    freeaddrinfo(res);
}

void sender_thread(int sock) {
    log_debug("Sender thread started");
    while (!all_ready) {
        for (size_t i = 0; i < hosts.size(); ++i) {
            if (hosts[i] != my_hostname && !ready_hosts[i]) {
                send_message(sock, "READY " + my_hostname, hosts[i]);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL_MS));
    }
    log_debug("Sender thread finished");
}

void receiver_thread(int sock) {
    log_debug("Receiver thread started");
    char buffer[1024];
    struct sockaddr_in sender_addr;
    socklen_t sender_addr_len = sizeof(sender_addr);

    while (!all_ready) {
        int bytes_received = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&sender_addr, &sender_addr_len);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';  // Null-terminate the received data
            log_debug("Received message: %s", buffer);
            std::string message(buffer);
            if (message.substr(0, 5) == "READY") {
                std::string sender_hostname = message.substr(6);
                auto it = std::find(hosts.begin(), hosts.end(), sender_hostname);
                if (it != hosts.end()) {
                    int index = std::distance(hosts.begin(), it);
                    std::lock_guard<std::mutex> lock(ready_mutex);
                    ready_hosts[index] = true;
                    
                    log_debug("Marking %s as ready", sender_hostname.c_str());
                    send_message(sock, "ACK " + my_hostname, sender_hostname);

                    if (std::all_of(ready_hosts.begin(), ready_hosts.end(), [](bool v) { return v; })) {
                        all_ready = true;
                        fprintf(stderr, "READY\n");
                        log_debug("All hosts are ready");
                    }
                }
            } else if (message.substr(0, 3) == "ACK") {
                std::string sender_hostname = message.substr(4);
                auto it = std::find(hosts.begin(), hosts.end(), sender_hostname);
                if (it != hosts.end()) {
                    int index = std::distance(hosts.begin(), it);
                    std::lock_guard<std::mutex> lock(ready_mutex);
                    ready_hosts[index] = true;
                    log_debug("Received ACK from %s", sender_hostname.c_str());
                }
            }
        }
    }
    log_debug("Receiver thread finished");
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <hostfile> <my_hostname>" << std::endl;
        return 1;
    }

    std::string hostfile = argv[1];
    my_hostname = argv[2];

    log_debug("Starting coordinator. Hostname: %s", my_hostname.c_str());

    hosts = read_hostfile(hostfile);
    ready_hosts.resize(hosts.size(), false);
    auto self_it = std::find(hosts.begin(), hosts.end(), my_hostname);
if (self_it != hosts.end()) {
    int self_index = std::distance(hosts.begin(), self_it);
    ready_hosts[self_index] = true;
    log_debug("Set own status (index %d) to ready", self_index);
}

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        log_debug("Failed to create socket. errno: %d", errno);
        return 1;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_debug("Failed to bind socket. errno: %d", errno);
        return 1;
    }

    log_debug("Socket bound successfully");

    // Add a delay to ensure all containers are up
    std::this_thread::sleep_for(std::chrono::seconds(5));
    log_debug("Starting coordination after initial delay");

    std::thread sender(sender_thread, sock);
    std::thread receiver(receiver_thread, sock);

    sender.join();
    receiver.join();

    close(sock);
    log_debug("Coordinator finished");
    return 0;
}