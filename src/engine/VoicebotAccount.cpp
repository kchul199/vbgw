#include "VoicebotAccount.h"

#include "../utils/AppConfig.h"
#include "SessionManager.h"
#include "VoicebotCall.h"

#include <pjlib.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <future>

using namespace pj;

VoicebotAccount::VoicebotAccount() {}

VoicebotAccount::~VoicebotAccount()
{
    // [L-1 Fix] 소멸자에서 mutex를 잡은 채 wait() → deadlock 가능성 제거
    // futures를 swap으로 꺼낸 뒤 mutex 해제 후 wait
    std::vector<std::future<void>> pending;
    {
        std::lock_guard<std::mutex> lock(futures_mutex_);
        pending.swap(answer_futures_);
    }
    for (auto& f : pending) {
        if (f.valid()) {
            f.wait();
        }
    }
}

void VoicebotAccount::onRegState(OnRegStateParam& prm)
{
    AccountInfo ai = getInfo();
    if (ai.regIsActive) {
        spdlog::info("[Account] Registered: {} (status={})", ai.uri, static_cast<int>(prm.code));
    } else {
        // [M-3 Fix] SIP 등록 해제 시 재등록 안내 로그
        // PJSIP는 AccountConfig의 regConfig.retryIntervalSec을 통해 자동 재등록을 지원하며,
        // 기본값이 0(재시도 안 함)이므로 create() 시 설정하는 것이 권장됨.
        // 현재는 경고를 상세하게 남겨 운영자가 인지할 수 있도록 함.
        spdlog::warn("[Account] Unregistered: {} (status={}) — PBX may be unreachable. "
                     "PJSIP will retry based on regConfig.retryIntervalSec setting.",
                     ai.uri, static_cast<int>(prm.code));
    }
}

void VoicebotAccount::onIncomingCall(OnIncomingCallParam& iprm)
{
    spdlog::info("[Account] Incoming SIP call, Call-ID: {}", iprm.callId);

    // [Phase3-M1 Fix] tryAddCall()으로 TOCTOU 방지 — canAcceptCall()+addCall() 분리 제거
    auto call = std::make_shared<VoicebotCall>(*this, iprm.callId);
    if (!SessionManager::getInstance().tryAddCall(iprm.callId, call)) {
        spdlog::warn("[Account] Max call limit reached. Rejecting call {} with 486 Busy Here.",
                     iprm.callId);
        CallOpParam prm;
        prm.statusCode = PJSIP_SC_BUSY_HERE;
        try {
            call->hangup(prm);
        } catch (...) {}
        return;
    }

    // 180 Ringing 전송 — PBX에 수신 알림
    try {
        CallOpParam ringing_prm;
        ringing_prm.statusCode = PJSIP_SC_RINGING;
        call->answer(ringing_prm);
        spdlog::info("[Account] Sent 180 Ringing for Call-ID: {}", iprm.callId);
    } catch (Error& err) {
        spdlog::error("[Account] Failed to send 180 Ringing: {}", err.info());
    }

    // [H-6 Fix] ANSWER_DELAY_MS를 AppConfig에서 캐싱된 값으로 읽기
    const int answer_delay_ms = AppConfig::instance().answer_delay_ms;
    const int call_id = iprm.callId;
    {
        std::lock_guard<std::mutex> lock(futures_mutex_);

        // 완료된 future 정리 (메모리 누적 방지)
        answer_futures_.erase(std::remove_if(answer_futures_.begin(), answer_futures_.end(),
                                             [](const std::future<void>& f) {
                                                 return f.wait_for(std::chrono::seconds(0)) ==
                                                        std::future_status::ready;
                                             }),
                              answer_futures_.end());

        // 200 OK 응답을 별도 스레드에서 비동기 처리
        answer_futures_.push_back(
            std::async(std::launch::async, [call, call_id, answer_delay_ms]() {
                // [CR-4 Fix] pj_thread_desc를 thread_local로 변경
                // 스택 로컬 pj_thread_desc는 std::async 태스크 완료 시 소멸하지만,
                // PJLIB 스레드 레지스트리는 여전히 이 메모리를 참조 → Use-After-Free
                // thread_local은 스레드 생존 기간 동안 유지되므로 UAF 방지
                thread_local pj_thread_desc thread_desc;
                thread_local pj_thread_t* pj_thread = nullptr;
                if (!pj_thread_is_registered()) {
                    pj_thread_register("vbgw_answer", thread_desc, &pj_thread);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(answer_delay_ms));
                try {
                    CallOpParam ok_prm;
                    ok_prm.statusCode = PJSIP_SC_OK;
                    call->answer(ok_prm);
                    spdlog::info("[Account] Sent 200 OK for Call-ID: {}", call_id);
                } catch (Error& err) {
                    spdlog::error("[Account] Failed to answer call {}: {}", call_id, err.info());
                    SessionManager::getInstance().removeCall(call_id);
                } catch (...) {
                    spdlog::error("[Account] Unknown error answering call {}", call_id);
                    SessionManager::getInstance().removeCall(call_id);
                }
            }));
    }
}
