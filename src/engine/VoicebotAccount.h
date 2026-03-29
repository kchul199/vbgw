#pragma once
#include <pjsua2.hpp>

#include <future>
#include <mutex>
#include <vector>

class VoicebotAccount : public pj::Account
{
public:
    VoicebotAccount();
    ~VoicebotAccount() override;

    virtual void onRegState(pj::OnRegStateParam& prm) override;
    virtual void onIncomingCall(pj::OnIncomingCallParam& iprm) override;

private:
    // [C-3 Fix] detach() 대신 future 보관 — 소멸자에서 완료 보장
    std::mutex futures_mutex_;
    std::vector<std::future<void>> answer_futures_;
};
