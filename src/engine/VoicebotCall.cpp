#include "VoicebotCall.h"

#include "../ai/VoicebotAiClient.h"
#include "../utils/AppConfig.h"
#include "SessionManager.h"
#include "VoicebotMediaPort.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <random>
#include <sstream>

// [M-9 Fix] UUID v4 생성기 — 분산 환경에서 세션 추적 가능
static std::string generateSessionId()
{
    static thread_local std::mt19937 gen(
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    // 간이 UUID v4: xxxxxxxx-xxxx-xxxx-xxxx
    std::ostringstream oss;
    oss << std::hex;
    oss << dist(gen) << "-";
    oss << (dist(gen) & 0xFFFF) << "-";
    oss << (dist(gen) & 0xFFFF) << "-";
    oss << (dist(gen) & 0xFFFF);
    return oss.str();
}

VoicebotCall::VoicebotCall(pj::Account& acc, int call_id)
    : pj::Call(acc, call_id), media_port_(nullptr), ai_client_(nullptr)
{
    start_time_ = std::chrono::system_clock::now();
}

VoicebotCall::~VoicebotCall()
{
    // ai_client_ endSession은 소멸자 호출 전 onCallState(DISCONNECTED)에서
    // SessionManager::removeCall()을 통해 shared_ptr refcount가 0이 되면 자동 수행됨
}

void VoicebotCall::onCallState(pj::OnCallStateParam& prm)
{
    pj::CallInfo ci = getInfo();
    spdlog::info("[Call] ID={} Session={} State={} Reason={}", ci.id, session_id_, ci.stateText,
                 ci.lastReason);

    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        dumpCdr(ci.lastReason);
        SessionManager::getInstance().removeCall(ci.id);
        spdlog::info("[Call] ID={} Session={} Removed from SessionManager.", ci.id, session_id_);
    }
}

void VoicebotCall::onCallMediaState(pj::OnCallMediaStateParam& prm)
{
    pj::CallInfo ci = getInfo();
    const auto& cfg = AppConfig::instance();

    for (unsigned i = 0; i < ci.media.size(); ++i) {
        if (ci.media[i].type != PJMEDIA_TYPE_AUDIO)
            continue;

        pj::AudioMedia* aud_med = dynamic_cast<pj::AudioMedia*>(getMedia(i));
        if (!aud_med)
            continue;

        bool needs_hangup = false;
        {
            std::lock_guard<std::mutex> lock(ai_init_mutex_);

            if (!ai_client_) {
                // [CR-1 Fix] gRPC 채널을 AppConfig 싱글톤에서 공유
                // 매 콜마다 TCP + HTTP/2 + TLS 핸드셰이크 비용 제거
                // AI_ENGINE_ADDR 검증도 AppConfig 생성 시 1회만 수행
                spdlog::info("[Call] Connecting to AI Engine at: {}", cfg.ai_engine_addr);

                try {
                    auto channel = cfg.getGrpcChannel();
                    ai_client_ = std::make_shared<VoicebotAiClient>(channel);
                } catch (const std::runtime_error& e) {
                    spdlog::critical("[Call] gRPC channel creation failed: {} — hanging up",
                                     e.what());
                    needs_hangup = true;
                }

                if (!needs_hangup) {
                    if (!cfg.grpc_use_tls) {
                        spdlog::warn(
                            "[Call] gRPC using INSECURE channel — GRPC_USE_TLS=1 is REQUIRED "
                            "for production");
                    }

                    // [M-1 Fix] this 캡처 → weak_from_this() 캡처
                    // AI 클라이언트의 read 스레드가 아직 실행 중일 때
                    // VoicebotCall이 먼저 소멸하면 dangling 접근 발생 방지
                    auto weak_self = weak_from_this();

                    ai_client_->setTtsCallback([weak_self](const uint8_t* data, size_t len) {
                        if (auto self = weak_self.lock()) {
                            if (self->media_port_) {
                                self->media_port_->writeTtsAudio(data, len);
                            }
                        }
                    });

                    ai_client_->setTtsClearCallback([weak_self]() {
                        if (auto self = weak_self.lock()) {
                            if (self->media_port_) {
                                self->media_port_->clearTtsAudio();
                                self->media_port_->resetVad();
                            }
                        }
                    });

                    ai_client_->setErrorCallback([weak_self](const std::string& err) {
                        spdlog::error("🚨 [Call] Hanging up due to permanent AI Error: {}", err);
                        if (auto self = weak_self.lock()) {
                            try {
                                pj::CallOpParam prm;
                                prm.statusCode = PJSIP_SC_SERVICE_UNAVAILABLE;
                                self->hangup(prm);
                            } catch (const pj::Error& e) {
                                spdlog::warn("[Call] Error during hangup: {}", e.info());
                            }
                        }
                    });

                    // [M-9 Fix] UUID 기반 세션 ID 생성
                    session_id_ = generateSessionId();
                    ai_client_->startSession(session_id_);
                }
            }

            if (!needs_hangup && !media_port_) {
                media_port_ = std::make_unique<VoicebotMediaPort>();
                media_port_->setAiClient(ai_client_);
            }
        }  // ai_init_mutex_ released

        if (needs_hangup) {
            try {
                pj::CallOpParam hangup_prm;
                hangup_prm.statusCode = PJSIP_SC_SERVICE_UNAVAILABLE;
                hangup(hangup_prm);
            } catch (...) {}
            return;
        }

        aud_med->startTransmit(*media_port_);
        media_port_->startTransmit(*aud_med);

        spdlog::info("[Call] AI Media Port connected. Session={} RTP Stream converting to PCM.",
                     session_id_);
    }
}

void VoicebotCall::onDtmfDigit(pj::OnDtmfDigitParam& prm)
{
    spdlog::info("[IVR] Session={} Received DTMF digit: {}", session_id_, prm.digit);
    // 향후 IVR 메뉴 전환 및 AI 엔진 우회 전송 로직 구현 지점
}

void VoicebotCall::dumpCdr(const std::string& reason)
{
    auto end_time = std::chrono::system_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time_).count();

    // 타임스탬프 포맷팅
    std::time_t end_time_t = std::chrono::system_clock::to_time_t(end_time);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&end_time_t));

    // [E-1 Fix] 운영 및 과금 목적의 CDR 구조화 로깅
    spdlog::info("[CDR] {{\"session_id\":\"{}\", "
                 "\"end_time\":\"{}\", "
                 "\"duration_sec\":{}, \"reason\":\"{}\", "
                 "\"vad_triggers\":{}, \"bargeins\":{}}}",
                 session_id_.empty() ? "N/A" : session_id_, buf, duration,
                 reason.empty() ? "Unknown" : reason, vad_trigger_count_.load(),
                 bargein_count_.load());
}
