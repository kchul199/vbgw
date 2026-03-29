#pragma once
#include <pjsua2.hpp>

#include <memory>

class VoicebotEndpoint
{
public:
    VoicebotEndpoint();
    ~VoicebotEndpoint();

    bool init();
    bool start(int sip_port);
    void shutdown();

private:
    std::unique_ptr<pj::Endpoint> ep_;
};
