#include "HttpServer.h"

#include "../engine/SessionManager.h"

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include <iostream>
#include <sstream>

using boost::asio::ip::tcp;

HttpServer::HttpServer() {}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::start(int port)
{
    if (is_running_)
        return false;
    is_running_ = true;

    server_thread_ = std::make_unique<std::thread>([this, port]() { runServer(port); });

    spdlog::info("[HttpServer] HTTP Admin Server starting on port {}", port);
    return true;
}

void HttpServer::stop()
{
    is_running_ = false;
    // 이 구현은 동기적 accept에 블로킹되므로 종료 시그널 처리가 어렵지만,
    // 데몬 종료 시 프로세스 단위로 내려가므로 문제없음.
    // 안전한 종료가 필요하면 async_accept로 전환.
    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->detach();  // 안전한 테스트를 위해 임시 분리
    }
}

void HttpServer::runServer(int port)
{
    try {
        boost::asio::io_context io_context;
        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));

        // Timeout 적용 등 상세 구성을 위해 accept
        while (is_running_) {
            tcp::socket socket(io_context);
            acceptor.accept(socket);

            // HTTP Request 라인 읽기
            boost::asio::streambuf request;
            boost::system::error_code error;
            boost::asio::read_until(socket, request, "\r\n\r\n", error);

            if (error && error != boost::asio::error::eof) {
                continue;
            }

            std::istream request_stream(&request);
            std::string req_method, req_path, req_version;
            request_stream >> req_method >> req_path >> req_version;

            std::string header_line;
            int content_length = 0;
            while (std::getline(request_stream, header_line) && header_line != "\r") {
                if (header_line.find("Content-Length:") == 0) {
                    content_length = std::stoi(header_line.substr(15));
                }
            }

            // HTTP Body 읽기
            std::string body;
            if (content_length > 0) {
                if (request.size() > 0) {
                    body.resize(request.size());
                    request_stream.read(&body[0], request.size());
                    content_length -= request.size();
                }
                if (content_length > 0) {
                    std::vector<char> body_buf(content_length);
                    boost::asio::read(socket, boost::asio::buffer(body_buf));
                    body.append(body_buf.begin(), body_buf.end());
                }
            }

            // 라우팅
            std::string response;
            if (req_method == "GET" && req_path == "/health") {
                response = handleHealthCheck();
            } else if (req_method == "GET" && req_path == "/metrics") {
                response = handleMetrics();
            } else if (req_method == "POST" && req_path == "/api/v1/calls") {
                response = handleOutboundCall(body);
            } else {
                response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            }

            boost::asio::write(socket, boost::asio::buffer(response));
            socket.close();
        }
    } catch (std::exception& e) {
        spdlog::error("[HttpServer] Server error: {}", e.what());
    }
}

std::string HttpServer::handleHealthCheck() const
{
    size_t calls = SessionManager::getInstance().getActiveCallCount();
    std::ostringstream ss;
    ss << "{\"status\":\"UP\", \"active_calls\":" << calls << "}";
    std::string json = ss.str();

    std::ostringstream res;
    res << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << json.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << json;

    return res.str();
}

std::string HttpServer::handleMetrics() const
{
    size_t calls = SessionManager::getInstance().getActiveCallCount();
    std::ostringstream ss;
    ss << "# HELP vbgw_active_calls Number of active voice calls\n"
       << "# TYPE vbgw_active_calls gauge\n"
       << "vbgw_active_calls " << calls << "\n";
    std::string body = ss.str();

    std::ostringstream res;
    res << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: text/plain; version=0.0.4\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;

    return res.str();
}

std::string HttpServer::handleOutboundCall(const std::string& request_body) const
{
    // 임시 토이 구현 (실제 프로덕션은 nlohmann/json 등 파서 연동)
    // body 예시: {"target_uri":"sip:1234@pbx.com"}
    spdlog::info("[HttpServer] Outbound Call Request: {}", request_body);

    // 실제 SessionManager의 makeCall을 시뮬레이트
    std::string json = "{\"status\":\"Accepted\", \"message\":\"Outbound call initiated.\"}";
    std::ostringstream res;
    res << "HTTP/1.1 202 Accepted\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << json.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << json;

    return res.str();
}
