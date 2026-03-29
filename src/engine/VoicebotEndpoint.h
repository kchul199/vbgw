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

    // [E-4] 코덱 우선순위 변경
    void setCodecPriority(const std::string& codec_id, short priority);

private:
    std::unique_ptr<pj::Endpoint> ep_;
};
