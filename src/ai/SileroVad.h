#pragma once
#include <vector>
#include <memory>
#include <string>

// Pimpl Idiom을 사용하여 ONNX 헤더 종속성을 캡슐화합니다.
class SileroVad {
public:
    SileroVad(); // 모델 경로를 지정하지 않으면 기본 "models/silero_vad.onnx" 탐색
    ~SileroVad();

    // 16kHz PCM(int16) 배열을 입력받아 화자 발화 점토(Probability)에 기반한 VAD 판별
    // threshold: 기본 0.5 (Silero 권장치)
    bool isSpeaking(const std::vector<int16_t>& pcm, float threshold = 0.5f);

    // 새로운 통화 시작 시 LSTM의 내부 상태(State)를 초기화
    void resetState();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
    std::vector<int16_t> pcm_buffer_;
    bool last_speaking_state_ = false;
};
