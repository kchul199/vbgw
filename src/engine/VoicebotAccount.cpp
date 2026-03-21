#include "VoicebotAccount.h"
#include "VoicebotCall.h"
#include "SessionManager.h"
#include <iostream>

using namespace pj;

VoicebotAccount::VoicebotAccount() {}
VoicebotAccount::~VoicebotAccount() {}

void VoicebotAccount::onRegState(OnRegStateParam &prm) {
    AccountInfo ai = getInfo();
    std::cout << (ai.regIsActive ? "Registered: " : "Unregistered: ")
              << ai.uri << " (status=" << prm.code << ")" << std::endl;
}

void VoicebotAccount::onIncomingCall(OnIncomingCallParam &iprm) {
    std::cout << "\nIncoming SIP call from Call-ID: " << iprm.callId << std::endl;
    
    // 1. 최대 채널 수 제한 확인 (OOM 및 리소스 낭비 방지)
    if (!SessionManager::getInstance().canAcceptCall()) {
        std::cout << "[방어 로직] 최대 허용 콜 수를 초과했습니다. 호를 거절(486)합니다." << std::endl;
        VoicebotCall rejected_call(*this, iprm.callId);
        CallOpParam prm;
        prm.statusCode = PJSIP_SC_BUSY_HERE; // 486 Busy Here (거절)
        try { rejected_call.hangup(prm); } catch(...) {}
        return;
    }

    // 2. 새로운 통화 세션 등록
    auto call = std::make_shared<VoicebotCall>(*this, iprm.callId);
    SessionManager::getInstance().addCall(iprm.callId, call);
    
    CallOpParam prm;
    prm.statusCode = PJSIP_SC_OK; // 200 OK 수락 (실서비스에서는 180 Ringing 후 STT/TTS 준비 시점 전송)
    try {
        call->answer(prm);
    } catch(Error& err) {
        std::cerr << "Failed to answer call: " << err.info() << std::endl;
        SessionManager::getInstance().removeCall(iprm.callId);
    }
}
