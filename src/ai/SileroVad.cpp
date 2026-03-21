#include "SileroVad.h"
#include <onnxruntime_cxx_api.h>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <filesystem>

struct SileroVad::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "SileroVAD"};
    Ort::SessionOptions session_options;
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;

    // VAD State Tensors (2, 1, 64) - Silero VAD v4 호환
    std::vector<float> h_state;
    std::vector<float> c_state;
    std::vector<float> input_tensor_values;
    int64_t sr = 16000;
    
    // ONNX 노드 이름
    std::vector<const char*> input_node_names = {"input", "sr", "h", "c"};
    std::vector<const char*> output_node_names = {"output", "hn", "cn"};

    Impl(const std::string& model_path) {
        if (!std::filesystem::exists(model_path)) {
            throw std::runtime_error("Silero VAD 모델을 찾을 수 없습니다: " + model_path);
        }

        session_options.SetIntraOpNumThreads(1);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        session = std::make_unique<Ort::Session>(env, model_path.c_str(), session_options);
        resetStates();
    }

    void resetStates() {
        h_state.assign(2 * 1 * 64, 0.0f);
        c_state.assign(2 * 1 * 64, 0.0f);
    }
};

SileroVad::SileroVad() : pimpl_(new Impl("models/silero_vad.onnx")) {}
SileroVad::~SileroVad() = default;

void SileroVad::resetState() {
    pimpl_->resetStates();
}

bool SileroVad::isSpeaking(const std::vector<int16_t>& pcm, float threshold) {
    if (pcm.empty()) return last_speaking_state_;

    // 지속적으로 512 샘플 단위(32ms)가 모였을 때만 ONNX 추론 실행
    pcm_buffer_.insert(pcm_buffer_.end(), pcm.begin(), pcm.end());
    
    // 512 샘플 길이 미만이면 마지막 상태(이전 결과) 유지 반환
    if (pcm_buffer_.size() < 512) {
        return last_speaking_state_;
    }
    
    // 딱 512개만 잘라서 Inference 수행
    std::vector<int16_t> chunk(pcm_buffer_.begin(), pcm_buffer_.begin() + 512);
    pcm_buffer_.erase(pcm_buffer_.begin(), pcm_buffer_.begin() + 512);

    // 16비트 PCM 정수를 float32(-1.0 ~ 1.0) 텐서 포맷으로 정규화 변환
    pimpl_->input_tensor_values.resize(chunk.size());
    for (size_t i = 0; i < chunk.size(); ++i) {
        pimpl_->input_tensor_values[i] = static_cast<float>(chunk[i]) / 32768.0f;
    }

    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // 1. input tensor (1, pcm_len)
    std::vector<int64_t> input_shape = {1, static_cast<int64_t>(chunk.size())};
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, pimpl_->input_tensor_values.data(), pimpl_->input_tensor_values.size(),
        input_shape.data(), input_shape.size());

    // 2. sr tensor (1)
    std::vector<int64_t> sr_shape = {1};
    Ort::Value sr_tensor = Ort::Value::CreateTensor<int64_t>(
        memory_info, &pimpl_->sr, 1, sr_shape.data(), sr_shape.size());

    // 3. h_state tensor (2, 1, 64)
    std::vector<int64_t> state_shape = {2, 1, 64};
    Ort::Value h_tensor = Ort::Value::CreateTensor<float>(
        memory_info, pimpl_->h_state.data(), pimpl_->h_state.size(),
        state_shape.data(), state_shape.size());

    // 4. c_state tensor (2, 1, 64)
    Ort::Value c_tensor = Ort::Value::CreateTensor<float>(
        memory_info, pimpl_->c_state.data(), pimpl_->c_state.size(),
        state_shape.data(), state_shape.size());

    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(input_tensor));
    inputs.push_back(std::move(sr_tensor));
    inputs.push_back(std::move(h_tensor));
    inputs.push_back(std::move(c_tensor));

    // 모델 추론 실행 (Inference)
    auto output_tensors = pimpl_->session->Run(
        Ort::RunOptions{nullptr}, 
        pimpl_->input_node_names.data(), 
        inputs.data(), inputs.size(), 
        pimpl_->output_node_names.data(), 3);

    // 내부 상태(h, c) 업데이트 복사
    float* hn = output_tensors[1].GetTensorMutableData<float>();
    std::memcpy(pimpl_->h_state.data(), hn, pimpl_->h_state.size() * sizeof(float));

    float* cn = output_tensors[2].GetTensorMutableData<float>();
    std::memcpy(pimpl_->c_state.data(), cn, pimpl_->c_state.size() * sizeof(float));

    // 결과 점수 획득
    float* output = output_tensors[0].GetTensorMutableData<float>();
    last_speaking_state_ = output[0] > threshold;
    
    return last_speaking_state_;
}
