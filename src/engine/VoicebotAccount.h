#pragma once
#include <pjsua2.hpp>

class VoicebotAccount : public pj::Account {
public:
    VoicebotAccount();
    ~VoicebotAccount();

    virtual void onRegState(pj::OnRegStateParam &prm) override;
    virtual void onIncomingCall(pj::OnIncomingCallParam &iprm) override;
};
