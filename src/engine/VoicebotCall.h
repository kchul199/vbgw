#pragma once
#include <pjsua2.hpp>
#include <memory>

class VoicebotAiClient;

class VoicebotCall : public pj::Call {
public:
    VoicebotCall(pj::Account &acc, int call_id = PJSUA_INVALID_ID);
    ~VoicebotCall();

    virtual void onCallState(pj::OnCallStateParam &prm) override;
    virtual void onCallMediaState(pj::OnCallMediaStateParam &prm) override;

private:
    class VoicebotMediaPort *media_port;
    std::shared_ptr<VoicebotAiClient> ai_client_;
};
