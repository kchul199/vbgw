#include "VoicebotEndpoint.h"
#include <iostream>

using namespace pj;

VoicebotEndpoint::VoicebotEndpoint() {
    ep.reset(new Endpoint);
}

VoicebotEndpoint::~VoicebotEndpoint() {
    shutdown();
}

bool VoicebotEndpoint::init() {
    try {
        ep->libCreate();
        EpConfig ep_cfg;
        // 로깅 설정
        ep_cfg.logConfig.level = 4;
        ep_cfg.logConfig.consoleLevel = 4;
        ep->libInit(ep_cfg);
        return true;
    } catch(Error& err) {
        std::cerr << "Initialization error: " << err.info() << std::endl;
        return false;
    }
}

void VoicebotEndpoint::start(int sip_port) {
    try {
        TransportConfig tcfg;
        tcfg.port = sip_port;
        ep->transportCreate(PJSIP_TRANSPORT_UDP, tcfg);
        ep->libStart();
        std::cout << "Voicebot SIP Endpoint started on port " << sip_port << std::endl;
    } catch(Error& err) {
        std::cerr << "Start error: " << err.info() << std::endl;
    }
}

void VoicebotEndpoint::shutdown() {
    try {
        ep->libDestroy();
    } catch(Error& err) {
        std::cerr << "Shutdown error: " << err.info() << std::endl;
    }
}
