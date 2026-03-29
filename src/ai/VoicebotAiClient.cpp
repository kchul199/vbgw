#include "VoicebotAiClient.h"

#include "../utils/AppConfig.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

using grpc::Channel;
using grpc::Status;
using voicebot::ai::AiResponse;
using voicebot::ai::AudioChunk;

// 편의상 stream 타입 alias
using Stream = grpc::ClientReaderWriter<AudioChunk, AiResponse>;

VoicebotAiClient::VoicebotAiClient(std::shared_ptr<Channel> channel)
    : stub_(voicebot::ai::VoicebotAiService::NewStub(channel)), is_running_(false)
{}

VoicebotAiClient::~VoicebotAiClient()
{
    endSession();
}

void VoicebotAiClient::setTtsCallback(std::function<void(const uint8_t*, size_t)> cb)
{
    on_tts_received_ = std::move(cb);
}

void VoicebotAiClient::setTtsClearCallback(std::function<void()> cb)
{
    on_tts_clear_ = std::move(cb);
}

void VoicebotAiClient::setErrorCallback(std::function<void(const std::string&)> cb)
{
    on_error_ = std::move(cb);
}

// [H-6 Fix] gRPC 스트림 최대 수명 deadline — AppConfig에서 캐싱된 값 사용
static std::chrono::system_clock::time_point makeStreamDeadline()
{
    int secs = AppConfig::instance().grpc_stream_deadline_secs;
    return std::chrono::system_clock::now() + std::chrono::seconds(secs);
}

void VoicebotAiClient::startSession(const std::string& session_id)
{
    current_session_id_ = session_id;

    auto new_context = std::make_unique<grpc::ClientContext>();
    new_context->set_deadline(makeStreamDeadline());
    auto new_stream = stub_->StreamSession(new_context.get());

    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        context_ = std::move(new_context);
        stream_ = std::move(new_stream);
    }

    is_running_.store(true, std::memory_order_release);
    reconnect_attempts_ = 0;
    worker_thread_ = std::thread(&VoicebotAiClient::streamWorker, this);
    read_thread_ = std::thread(&VoicebotAiClient::readWorker, this);

    spdlog::info("[gRPC] AI Stream Session started for: {}", session_id);
}

void VoicebotAiClient::sendAudio(const std::vector<uint8_t>& pcm_data, bool is_speaking)
{
    sendAudio(pcm_data.data(), pcm_data.size(), is_speaking);
}

void VoicebotAiClient::sendAudio(const uint8_t* data, size_t len, bool is_speaking)
{
    if (!is_running_.load(std::memory_order_acquire))
        return;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        if (audio_queue_.size() >= kMaxAudioQueueSize) {
            spdlog::warn("[gRPC] Audio queue full ({} frames). Dropping frame for session: {}",
                         kMaxAudioQueueSize, current_session_id_);
            return;
        }

        audio_queue_.push({std::vector<uint8_t>(data, data + len), is_speaking});
    }
    queue_cv_.notify_one();
}

void VoicebotAiClient::streamWorker()
{
    while (is_running_.load(std::memory_order_acquire)) {
        AudioItem item;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !audio_queue_.empty() || !is_running_.load(std::memory_order_acquire);
            });

            if (!is_running_.load(std::memory_order_acquire) && audio_queue_.empty())
                break;

            item = std::move(audio_queue_.front());
            audio_queue_.pop();
        }

        std::shared_ptr<Stream> local_stream;
        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            local_stream = stream_;
        }

        if (local_stream) {
            AudioChunk chunk;
            chunk.set_session_id(current_session_id_);
            chunk.set_audio_data(item.pcm_data.data(), item.pcm_data.size());
            chunk.set_is_speaking(item.is_speaking);

            if (!local_stream->Write(chunk)) {
                spdlog::warn("[gRPC] Stream write failed for session: {}. Triggering reconnect.",
                             current_session_id_);
                std::lock_guard<std::mutex> lock(stream_mutex_);
                if (context_)
                    context_->TryCancel();
            }
        }
    }
    spdlog::debug("[gRPC] streamWorker exited for session: {}", current_session_id_);
}

void VoicebotAiClient::endSession()
{
    // 이미 정지된 상태라면 중복 수행 방지
    bool expected = true;
    if (!is_running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        return;

    queue_cv_.notify_all();

    // [H-2 Fix] endSession() 스트림 정리 개선
    // TryCancel() 후 stream은 이미 CANCELLED 상태이므로
    // WritesDone()/Finish() 호출은 불필요하며 블로킹될 수 있음
    bool was_cancelled = false;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (context_) {
            context_->TryCancel();
            was_cancelled = true;
        }
    }

    if (worker_thread_.joinable())
        worker_thread_.join();
    if (read_thread_.joinable())
        read_thread_.join();

    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_) {
            // [H-2 Fix] TryCancel 된 스트림에는 WritesDone/Finish를 호출하지 않음
            // — gRPC 내부적으로 블로킹되거나 무의미한 에러를 반환할 수 있음
            if (!was_cancelled) {
                try {
                    stream_->WritesDone();
                    Status status = stream_->Finish();
                    if (status.ok()) {
                        spdlog::info("[gRPC] Stream closed successfully for session: {}",
                                     current_session_id_);
                    } else {
                        spdlog::warn("[gRPC] Stream finished with status: {}",
                                     status.error_message());
                    }
                } catch (...) {
                    spdlog::warn("[gRPC] Exception during stream cleanup for session: {}",
                                 current_session_id_);
                }
            } else {
                spdlog::debug("[gRPC] Stream was cancelled — skipping WritesDone/Finish for: {}",
                              current_session_id_);
            }
            stream_.reset();
        }
    }
}

bool VoicebotAiClient::tryConnectAndRead()
{
    std::shared_ptr<Stream> local_stream;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        local_stream = stream_;
    }

    if (!local_stream)
        return is_running_.load(std::memory_order_acquire);

    AiResponse response;
    while (is_running_.load(std::memory_order_acquire) && local_stream->Read(&response)) {
        if (response.type() == AiResponse::TTS_AUDIO) {
            const std::string& audio = response.audio_data();
            if (on_tts_received_ && !audio.empty()) {
                on_tts_received_(reinterpret_cast<const uint8_t*>(audio.data()), audio.size());
            }
        } else if (response.type() == AiResponse::STT_RESULT) {
            spdlog::info("[AI STT] User ({}): {}", current_session_id_, response.text_content());
        } else if (response.type() == AiResponse::END_OF_TURN) {
            if (response.clear_buffer() && on_tts_clear_) {
                spdlog::warn("🚨 [Barge-In] Flushed Gateway TTS RingBuffer! Session: {}",
                             current_session_id_);
                on_tts_clear_();
            }
        }
        // 정상 수신마다 재연결 카운터 초기화
        reconnect_attempts_ = 0;
    }

    return is_running_.load(std::memory_order_acquire);
}

void VoicebotAiClient::readWorker()
{
    while (is_running_.load(std::memory_order_acquire)) {
        bool needs_reconnect = tryConnectAndRead();

        if (!needs_reconnect || !is_running_.load(std::memory_order_acquire))
            break;

        if (reconnect_attempts_ >= kMaxReconnectRetries) {
            spdlog::error("[gRPC] Max reconnect retries ({}) exceeded. Triggering error callback.",
                          kMaxReconnectRetries);
            if (on_error_)
                on_error_("gRPC STT/TTS Stream permanently disconnected after retries");
            return;
        }

        // 지수 백오프: 500ms * 2^n (최대 16초)
        int wait_ms = std::min(500 * (1 << reconnect_attempts_.load()), 16000);
        reconnect_attempts_++;
        spdlog::warn("[gRPC] Stream disconnected. Reconnecting in {}ms (attempt {}/{})", wait_ms,
                     reconnect_attempts_.load(), kMaxReconnectRetries);

        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));

        auto new_context = std::make_unique<grpc::ClientContext>();
        new_context->set_deadline(makeStreamDeadline());
        auto new_stream = stub_->StreamSession(new_context.get());

        if (!new_stream) {
            spdlog::error("[gRPC] Failed to create new stream. Aborting reconnect.");
            if (on_error_)
                on_error_("gRPC stream re-creation failed");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            context_ = std::move(new_context);
            stream_ = std::move(new_stream);
        }

        spdlog::info("[gRPC] Reconnected stream for session: {}", current_session_id_);
    }
    spdlog::debug("[gRPC] readWorker exited for session: {}", current_session_id_);
}
