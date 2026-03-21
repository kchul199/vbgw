#include <spdlog/spdlog.h>
#include "VoicebotMediaPort.h"
#include "../utils/RingBuffer.h"
#include "../ai/VoicebotAiClient.h"
#include "../ai/SileroVad.h"
#include <iostream>

using namespace pj;

VoicebotMediaPort::VoicebotMediaPort() : AudioMediaPort(), vad_(new SileroVad()) {
    // AI 모델(STT/TTS)을 위해 16kHz, 1채널 16비트 PCM (20ms) 코덱 포맷 설정
    MediaFormatAudio fmt;
    fmt.type = PJMEDIA_TYPE_AUDIO;
    fmt.clockRate = 16000;
    fmt.channelCount = 1;
    fmt.bitsPerSample = 16;
    fmt.frameTimeUsec = 20000; // 20 milliseconds

    createPort("VoicebotMediaPort", fmt);
    
    // 16kHz, 16bit, 1채널 5초 분량 버퍼 = 16000 * 2 * 5 = 160000 바이트
    tts_buffer_ = new RingBuffer(160000);
    std::cout << "Custom Media Port Initialized (16kHz PCM)" << std::endl;
}

VoicebotMediaPort::~VoicebotMediaPort() {
    delete tts_buffer_;
    delete vad_;
}

void VoicebotMediaPort::setAiClient(std::shared_ptr<VoicebotAiClient> client) {
    std::lock_guard<std::mutex> lock(client_mutex_);
    ai_client_ = client;
}

void VoicebotMediaPort::onFrameReceived(MediaFrame &frame) {
    std::shared_ptr<VoicebotAiClient> safe_client;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        safe_client = ai_client_;
    }

    if (frame.type == PJMEDIA_FRAME_TYPE_AUDIO && frame.size > 0 && safe_client) {
        std::vector<uint8_t> pcm((uint8_t*)frame.buf.data(), (uint8_t*)frame.buf.data() + frame.size);
        
        // 실시간 Edge AI VAD 연산 (Silero ONNX)
        const int16_t* pcm16 = reinterpret_cast<const int16_t*>(frame.buf.data());
        size_t samples = frame.size / 2;
        std::vector<int16_t> pcm_vec(pcm16, pcm16 + samples);
        
        bool is_speaking = vad_->isSpeaking(pcm_vec);
        
        safe_client->sendAudio(pcm, is_speaking);
    }
}

void VoicebotMediaPort::onFrameRequested(MediaFrame &frame) {
    frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
    if (frame.buf.size() > 0) {
        // 링버퍼에서 읽어오기
        size_t read_bytes = tts_buffer_->read((uint8_t*)frame.buf.data(), frame.buf.size());
        
        // 데이터가 부족하면 나머지를 0(Silence)으로 묵음 처리하여 끊김 잡음(Pop) 방지
        if (read_bytes < frame.buf.size()) {
            std::memset((uint8_t*)frame.buf.data() + read_bytes, 0, frame.buf.size() - read_bytes);
        }
        frame.size = frame.buf.size();
    } else {
        frame.size = 0;
    }
}

void VoicebotMediaPort::writeTtsAudio(const uint8_t* data, size_t len) {
    tts_buffer_->write(data, len);
}

void VoicebotMediaPort::clearTtsAudio() {
    tts_buffer_->clear();
}

void VoicebotMediaPort::resetVad() {
    if (vad_) {
        vad_->resetState();
    }
}
