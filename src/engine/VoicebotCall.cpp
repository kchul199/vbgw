#include "VoicebotCall.h"
#include "VoicebotMediaPort.h"
#include "SessionManager.h"
#include "../ai/VoicebotAiClient.h"
#include <grpcpp/grpcpp.h>
#include <iostream>

using namespace pj;

VoicebotCall::VoicebotCall(Account &acc, int call_id)
    : Call(acc, call_id), media_port(nullptr), ai_client_(nullptr) {}

VoicebotCall::~VoicebotCall() {
    if (media_port) {
        delete media_port;
        media_port = nullptr;
    }
}

void VoicebotCall::onCallState(OnCallStateParam &prm) {
    CallInfo ci = getInfo();
    std::cout << "Call " << ci.id << " state: " << ci.stateText << std::endl;

    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        std::cout << "Call disconnected. Reason: " << ci.lastReason << std::endl;
        // Delete this call instance safely via SessionManager
        SessionManager::getInstance().removeCall(ci.id);
    }
}

void VoicebotCall::onCallMediaState(OnCallMediaStateParam &prm) {
    CallInfo ci = getInfo();
    for (unsigned i = 0; i < ci.media.size(); ++i) {
        if (ci.media[i].type == PJMEDIA_TYPE_AUDIO && getMedia(i)) {
            AudioMedia *aud_med = (AudioMedia *)getMedia(i);
            
            // 1. gRPC 클라이언트 (AI 엔진 접속) 세팅 및 링버퍼 콜백 연결
            if (!ai_client_) {
                auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
                ai_client_ = std::make_shared<VoicebotAiClient>(channel);
                
                ai_client_->setTtsCallback([this](const uint8_t* data, size_t len) {
                    if (media_port) {
                        media_port->writeTtsAudio(data, len);
                    }
                });
                
                ai_client_->setTtsClearCallback([this]() {
                    if (media_port) {
                        media_port->clearTtsAudio();
                        media_port->resetVad(); // 화자 개입 시 VAD 히스토리 초기화
                    }
                });
                
                ai_client_->setErrorCallback([this](const std::string& err) {
                    std::cerr << "🚨 [Call] Hanging up due to AI Error: " << err << std::endl;
                    // AI 서버 장애 시 통화 강제 종류 (503 Service Unavailable)
                    try {
                        pj::CallOpParam prm;
                        prm.statusCode = PJSIP_SC_SERVICE_UNAVAILABLE;
                        hangup(prm);
                    } catch (const pj::Error& e) {
                        // ignore error
                    }
                });
                
                char session_id_str[32];
                snprintf(session_id_str, sizeof(session_id_str), "%d", getInfo().id);
                ai_client_->startSession(session_id_str);
            }

            // 2. 미디어 포트를 콜 스트림에 물리며 앞서 생성한 AI 클라이언트 주입
            if (!media_port) {
                media_port = new VoicebotMediaPort();
                media_port->setAiClient(ai_client_);
            }
            
            // Call 오디오 파이프라인 <-> Custom Port 연결 (Rx/Tx)
            aud_med->startTransmit(*media_port);
            media_port->startTransmit(*aud_med);
            
            std::cout << "AI Media Port connected. RTP Stream converting to PCM." << std::endl;
        }
    }
}
