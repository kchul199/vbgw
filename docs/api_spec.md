# gRPC API Specification

Voicebot Gateway (VBGW)와 AI Engine(STT/TTS) 간의 통신을 위한 gRPC API 규격서입니다.

## 1. 서비스 개요 (Service Overview)

VBGW는 AI 엔진과 **Bi-directional Streaming** 방식을 사용하여 음성 데이터와 텍스트(STT/TTS) 이벤트를 실시간으로 주고받습니다.

- **Proto File**: `protos/voicebot.proto`
- **Service Name**: `voicebot.ai.VoicebotAiService`
- **RPC Method**: `StreamSession`

## 2. 메시지 규격 (Message Specifications)

### 2.1 AudioChunk (Gateway -> AI Engine)
콜봇 게이트웨이에서 AI 엔진으로 전달되는 오디오 데이터 및 상태 정보입니다.

| 필드명 | 타입 | 설명 |
| :--- | :--- | :--- |
| `session_id` | `string` | PJSIP에서 생성된 고유 Call ID |
| `audio_data` | `bytes` | 16kHz, 16bit, Mono PCM 바이너리 (최소 20ms 단위) |
| `is_speaking` | `bool` | 게이트웨이 내부 Silero VAD에 의한 음성 감지 여부 |

### 2.2 AiResponse (AI Engine -> Gateway)
AI 엔진에서 게이트웨이로 전달되는 처리 결과 및 음성 데이터입니다.

| 필드명 | 타입 | 설명 |
| :--- | :--- | :--- |
| `type` | `enum` | 응답 유형 (`STT_RESULT`, `TTS_AUDIO`, `END_OF_TURN`) |
| `text_content` | `string` | STT 인식 결과 텍스트 또는 TTS 원문 텍스트 |
| `audio_data` | `bytes` | Gateway가 재생해야 할 16kHz PCM 바이너리 |
| `clear_buffer` | `bool` | `true`일 경우 게이트웨이의 TTS 재생 버퍼를 즉시 비움 (Barge-in 처리) |

## 3. 통신 흐름 (Communication Flow)

1.  **세션 시작**: VBGW가 `StreamSession` 호출을 시작합니다.
2.  **데이터 송신**: VBGW는 RTP로 수신된 고객 음성을 PCM으로 변환하여 실시간으로 `AudioChunk`를 전송합니다.
3.  **결과 수신**: AI 엔진은 음성을 분석하여 `STT_RESULT`를 보내고, 답변이 준비되면 `TTS_AUDIO`를 스트리밍합니다.
4.  **Barge-in**: 사용자가 AI 답변 중 말을 시작하면, AI 엔진은 `END_OF_TURN`과 함께 `clear_buffer=true`를 보내어 게이트웨이의 재생을 중단시킵니다.
5.  **세션 종료**: 통화가 종료되면 VBGW가 `WritesDone`을 호출하고 스트림을 닫습니다.
