#include "VoicebotAiClient.h"
#include <iostream>

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReaderWriter;
using grpc::Status;
using voicebot::ai::AudioChunk;
using voicebot::ai::AiResponse;

VoicebotAiClient::VoicebotAiClient(std::shared_ptr<Channel> channel)
    : stub_(voicebot::ai::VoicebotAiService::NewStub(channel)), is_running_(false) {
}

VoicebotAiClient::~VoicebotAiClient() {
    endSession();
}

void VoicebotAiClient::setTtsCallback(std::function<void(const uint8_t*, size_t)> cb) {
    on_tts_received_ = cb;
}

void VoicebotAiClient::setTtsClearCallback(std::function<void()> cb) {
    on_tts_clear_ = cb;
}

void VoicebotAiClient::setErrorCallback(std::function<void(const std::string&)> cb) {
    on_error_ = cb;
}

void VoicebotAiClient::startSession(const std::string& session_id) {
    current_session_id_ = session_id;
    // gRPC 양방향 스트림 채널 열기
    stream_ = stub_->StreamSession(&context_);
    
    is_running_ = true;
    worker_thread_ = std::thread(&VoicebotAiClient::streamWorker, this);
    read_thread_ = std::thread(&VoicebotAiClient::readWorker, this);

    std::cout << "[gRPC] AI Stream Session started for: " << session_id << std::endl;
}

void VoicebotAiClient::sendAudio(const std::vector<uint8_t>& pcm_data, bool is_speaking) {
    if (!stream_) return;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        audio_queue_.push(pcm_data);
    }
    queue_cv_.notify_one();
}

void VoicebotAiClient::streamWorker() {
    while (is_running_) {
        std::vector<uint8_t> pcm_data;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !audio_queue_.empty() || !is_running_; });
            
            if (!is_running_ && audio_queue_.empty()) break;
            
            pcm_data = std::move(audio_queue_.front());
            audio_queue_.pop();
        }

        if (stream_) {
            AudioChunk chunk;
            chunk.set_session_id(current_session_id_);
            chunk.set_audio_data(pcm_data.data(), pcm_data.size());
            chunk.set_is_speaking(true); // VAD logic could modify this

            if (!stream_->Write(chunk)) {
                std::cerr << "[gRPC] Async stream write failed." << std::endl;
            }
        }
    }
}

void VoicebotAiClient::endSession() {
    is_running_ = false;
    queue_cv_.notify_all();

    if (stream_) {
        // Send EOF to STT Server
        stream_->WritesDone();
    }
    
    if (worker_thread_.joinable()) worker_thread_.join();
    if (read_thread_.joinable()) read_thread_.join();

    if (stream_) {
        Status status = stream_->Finish();
        if (status.ok()) {
            std::cout << "[gRPC] Stream closed successfully." << std::endl;
        } else {
            std::cerr << "[gRPC] Stream failed: " << status.error_message() << std::endl;
        }
        stream_.reset();
    }
}

void VoicebotAiClient::readWorker() {
    AiResponse response;
    while (is_running_ && stream_ && stream_->Read(&response)) {
        if (response.type() == AiResponse::TTS_AUDIO) {
            const std::string& audio = response.audio_data();
            if (on_tts_received_ && !audio.empty()) {
                on_tts_received_(reinterpret_cast<const uint8_t*>(audio.data()), audio.size());
            }
        }
        else if (response.type() == AiResponse::STT_RESULT) {
            std::cout << "\n[AI STT] User (" << current_session_id_ << "): " << response.text_content() << std::endl;
        }
        else if (response.type() == AiResponse::END_OF_TURN) {
            if (response.clear_buffer() && on_tts_clear_) {
                std::cout << "🚨 [Barge-In] Flushed Gateway TTS RingBuffer!" << std::endl;
                on_tts_clear_();
            }
        }
    }
    
    // 만약 정상 종료(is_running_=false)가 아닌데 스트림이 끊어지면 장애로 간주
    if (is_running_ && on_error_) {
        on_error_("gRPC STT/TTS Stream disconnected unexpectedly");
    }
}
