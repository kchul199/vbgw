#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

// 전방 선언 (Boost 헤더 꼬임 방지)
namespace boost {
namespace asio {
class io_context;
}
}  // namespace boost

class HttpServer
{
public:
    static HttpServer& getInstance()
    {
        static HttpServer instance;
        return instance;
    }

    // 서버 시작 (백그라운드 스레드에서 수신 대기)
    bool start(int port);

    // 서버 중지
    void stop();

private:
    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void runServer(int port);
    std::string handleRequest(const std::string& request_line, const std::string& body);

    // 엔드포인트 핸들러
    std::string handleHealthCheck() const;
    std::string handleMetrics() const;
    std::string handleOutboundCall(const std::string& json_body) const;

    std::unique_ptr<std::thread> server_thread_;
    std::atomic<bool> is_running_{false};

    // io_context는 pimpl이나 포인터로 숨기는 것이 좋지만 단순화를 위해 내부에 생성
};
