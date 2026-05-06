#include "xray_propagator.h"

#include <iostream>
#include <string>
#include <sstream>
#include <chrono>
#include <random>
#include <iomanip>
#include <cstring>
#include <thread>
#include <atomic>
#include <csignal>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <curl/curl.h>

static std::atomic<bool> running{true};

void signal_handler(int) { running = false; }

static std::string random_hex(int bytes) {
    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    for (int i = 0; i < bytes; i += 8) {
        uint64_t val = dist(rng);
        int remaining = std::min(8, bytes - i);
        for (int j = 0; j < remaining; j++) {
            oss << std::hex << std::setfill('0') << std::setw(2)
                << ((val >> (j * 8)) & 0xFF);
        }
    }
    return oss.str();
}

static int64_t now_nanos() {
    auto tp = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
}

static double now_seconds() {
    auto tp = std::chrono::system_clock::now();
    return std::chrono::duration<double>(tp.time_since_epoch()).count();
}

static size_t curl_write_cb(void*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

static void send_span_to_collector(
    const std::string& trace_id,
    const std::string& span_id,
    const std::string& parent_span_id,
    const std::string& name,
    int64_t start_ns,
    int64_t end_ns,
    bool sampled) {

    if (!sampled) return;

    std::ostringstream json;
    json << R"({"resourceSpans":[{"resource":{"attributes":[)"
         << R"({"key":"service.name","value":{"stringValue":"demo-backend-cpp"}},)"
         << R"({"key":"service.namespace","value":{"stringValue":"webinar-demo"}},)"
         << R"({"key":"deployment.environment","value":{"stringValue":"production"}})"
         << R"(]},"scopeSpans":[{"scope":{"name":"demo-backend-cpp","version":"1.0.0"},"spans":[{)"
         << R"("traceId":")" << trace_id << R"(",)"
         << R"("spanId":")" << span_id << R"(",)";

    if (!parent_span_id.empty()) {
        json << R"("parentSpanId":")" << parent_span_id << R"(",)";
    }

    json << R"("name":")" << name << R"(",)"
         << R"("kind":2,)"
         << R"("startTimeUnixNano":")" << start_ns << R"(",)"
         << R"("endTimeUnixNano":")" << end_ns << R"(",)"
         << R"("attributes":[)"
         << R"({"key":"http.method","value":{"stringValue":"GET"}},)"
         << R"({"key":"http.route","value":{"stringValue":"/demo"}},)"
         << R"({"key":"http.status_code","value":{"intValue":"200"}})"
         << R"(],)"
         << R"("status":{"code":1})"
         << R"(}]}]}]})";

    std::string payload = json.str();

    CURL* curl = curl_easy_init();
    if (!curl) return;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:4318/v1/traces");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

static std::unordered_map<std::string, std::string> parse_headers(const std::string& raw) {
    std::unordered_map<std::string, std::string> headers;
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(' '));
            headers[key] = value;
        }
    }
    return headers;
}

static std::string handle_request(const std::string& raw_request) {
    auto header_end = raw_request.find("\r\n\r\n");
    std::string header_section = (header_end != std::string::npos)
        ? raw_request.substr(0, header_end)
        : raw_request;

    auto headers = parse_headers(header_section);
    auto xray_ctx = AwsXRayPropagator::extract(headers);

    std::string trace_id;
    std::string parent_span_id;
    bool sampled = true;

    if (xray_ctx.has_value()) {
        trace_id = xray_ctx->trace_id;
        parent_span_id = xray_ctx->parent_span_id;
        sampled = xray_ctx->sampled;
    } else {
        trace_id = random_hex(16);
    }

    std::string span_id = random_hex(8);
    int64_t start_ns = now_nanos();

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    int64_t end_ns = now_nanos();

    std::thread([=]() {
        send_span_to_collector(trace_id, span_id, parent_span_id,
                               "GET /demo", start_ns, end_ns, sampled);
    }).detach();

    std::ostringstream body;
    body << R"({"service":"demo-backend-cpp","status":"ok","timestamp":)"
         << std::fixed << std::setprecision(6) << now_seconds()
         << R"(,"trace_id":")" << trace_id << R"("})";

    std::string body_str = body.str();
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << body_str.size() << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << body_str;

    return response.str();
}

static std::string handle_health() {
    std::string body = R"({"status":"healthy"})";
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << body;
    return response.str();
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8082);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind to port 8082\n";
        close(server_fd);
        return 1;
    }

    listen(server_fd, 16);
    std::cout << "demo-backend-cpp listening on port 8082\n";

    struct timeval tv{};
    tv.tv_sec = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (running) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        char buf[4096] = {};
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            std::string raw(buf, n);
            std::string response;

            if (raw.find("GET /health") != std::string::npos) {
                response = handle_health();
            } else if (raw.find("GET /demo") != std::string::npos) {
                response = handle_request(raw);
            } else {
                response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            }

            write(client_fd, response.c_str(), response.size());
        }
        close(client_fd);
    }

    close(server_fd);
    curl_global_cleanup();
    std::cout << "Shutting down\n";
    return 0;
}
