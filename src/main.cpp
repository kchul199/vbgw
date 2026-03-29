#include "engine/SessionManager.h"
#include "engine/VoicebotAccount.h"
#include "engine/VoicebotEndpoint.h"
#include "utils/AppConfig.h"

#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

// 전역 종료 플래그 (SIGINT 등 인터럽트 기반 안전 종료 대기용)
std::atomic<bool> keep_running(true);

// [CR-2 Fix] 시그널 핸들러: async-signal-safe 함수만 사용
// std::cout/cerr은 async-signal-safe가 아니므로 제거
void signalHandler(int /* signum */)
{
    keep_running.store(false, std::memory_order_release);
}

int main()
{
    // [CR-2 Fix] signal() → sigaction() 전환
    // signal()은 POSIX에서 동작이 구현체 의존(UB)이며,
    // 핸들러 실행 중 동일 시그널 재수신 시 경쟁 조건 발생
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // [H-6 Fix] AppConfig 싱글톤 초기화 — 모든 환경변수를 1회 읽어 캐싱
    const auto& cfg = AppConfig::instance();

    // [O4-1] 멀티싱크 로거: 콘솔 + 파일 (LOG_DIR 환경변수 설정 시 활성)
    // 파일 로그: 10MB 롤링, 최대 5개 보관
    auto log_level = spdlog::level::from_str(cfg.log_level);

    std::vector<spdlog::sink_ptr> sinks;
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    sinks.push_back(console_sink);

    if (!cfg.log_dir.empty()) {
        std::filesystem::create_directories(cfg.log_dir);
        std::string log_path = cfg.log_dir + "/vbgw.log";
        auto file_sink =
            std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_path, 10 * 1024 * 1024, 5);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("vbgw", sinks.begin(), sinks.end());
    logger->set_level(log_level);
    spdlog::set_default_logger(logger);

    spdlog::info("Starting AI Voicebot Gateway (PJSUA2)... [log_level={}{}]", cfg.log_level,
                 cfg.log_dir.empty() ? "" : std::string(", log_dir=") + cfg.log_dir);

    VoicebotEndpoint ep;
    if (!ep.init()) {
        spdlog::critical("Initialization failed. Aborting.");
        return 1;
    }

    if (!ep.start(cfg.sip_port)) {
        return 1;
    }

    pj::AccountConfig acc_cfg;

    if (cfg.pbx_mode) {
        acc_cfg.idUri = cfg.pbx_id_uri;
        acc_cfg.regConfig.registrarUri = cfg.pbx_uri;
        acc_cfg.sipConfig.authCreds.push_back(
            pj::AuthCredInfo("digest", "*", cfg.pbx_username, 0, cfg.pbx_password));
        spdlog::info("[VBGW] PBX Registration Mode Enabled.");
        spdlog::info("       - Registrar: {}", cfg.pbx_uri);
        spdlog::info("       - ID URI: {}", cfg.pbx_id_uri);
    } else {
        acc_cfg.idUri = "sip:voicebot@127.0.0.1";
        spdlog::info("[VBGW] Local Mode Enabled (No PBX). Direct IP calls: {}", acc_cfg.idUri);
    }

    // 계정 생성 및 PBX REG 등록
    VoicebotAccount acc;
    try {
        acc.create(acc_cfg);
        spdlog::info("Voicebot Account created. Listening or Registered to PBX successfully!");
    } catch (pj::Error& err) {
        spdlog::error("Error creating account: {}", err.info());
        return 1;
    }

    spdlog::info("Press Ctrl+C to stop the gateway gracefully.");

    // 데몬 루프
    while (keep_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 1. 진행 중인 모든 통화 강제 종료 (PBX에 BYE 전송)
    spdlog::info("[Shutdown 1/3] Hanging up all active calls...");
    SessionManager::getInstance().hangupAllCalls();

    // 네트워크 상으로 BYE 패킷이 안전하게 전송될 시간을 확보
    int max_wait_ms = 5000;
    while (SessionManager::getInstance().getActiveCallCount() > 0 && max_wait_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        max_wait_ms -= 100;
    }

    // 2. 로컬 SIP 포트 닫기 및 계정 정리
    spdlog::info("[Shutdown 2/3] Destroying account...");
    try {
        acc.modify(acc_cfg);
    } catch (pj::Error& err) {
        spdlog::warn("[Shutdown] Account modify error (non-fatal): {}", err.info());
    }

    // 3. PJSIP 엔진 완벽히 내리기
    spdlog::info("[Shutdown 3/3] Shutting down PJLIB...");
    ep.shutdown();

    spdlog::info("Gateway shutdown complete. Goodbye!");
    return 0;
}
