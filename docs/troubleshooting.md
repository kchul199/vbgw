# Troubleshooting Guide

Voicebot Gateway(VBGW) 운영 중 발생 가능한 문제와 해결 방법입니다.

## 1. 빌드 및 설치 관련 (Build & Installation)

### 1.1 ONNX Runtime 라이브러리 미인식
- **현상**: `find_package` 또는 `find_library`에서 `onnxruntime`을 찾지 못함.
- **해결**: `CMakeLists.txt`에서 `onnxruntime` 경로가 `/opt/homebrew/lib` (M칩 맥 기준)으로 설정되어 있는지 확인하십시오. 다른 경로에 설치된 경우 `PATHS`를 수정해야 합니다.

### 1.2 PJSIP(PJPROJECT) 의존성 오류
- **현상**: 컴파일 시 `pjsua.h` 헤더를 찾을 수 없음.
- **해결**: `brew install pjproject`가 정상적으로 완료되었는지 확인하고, `pkg-config --cflags libpjproject` 명령어로 경로가 출력되는지 점검하십시오.

## 2. 런타임 오류 (Runtime Issues)

### 2.1 VAD 모델 로드 실패
- **현상**: 실행 시 `Silero VAD 모델을 찾을 수 없습니다` 에러 발생.
- **해결**: `models/silero_vad.onnx` 파일이 프로젝트 루트 하위에 존재하는지 확인하십시오. 실행 시 작업 디렉토리(CWD)가 프로젝트 루트여야 합니다.

### 2.2 gRPC 스트림 연결 실패
- **현상**: `[gRPC] Stream failed: Deadline Exceeded` 또는 연결 거부.
- **해결**: AI 엔진(에뮬레이터 등)이 `localhost:50051`에서 정상 동작 중인지 확인하십시오. 방화벽 설정에 의해 포트가 차단되지 않았는지 점검하십시오.

### 2.3 음성 끊김 또는 지연 (Audio Stuttering/Latency)
- **현상**: 사용자의 음성이 AI에게 느게 전달되거나 지터 발생.
- **해결**: 
    - 네트워크 환경이 RTP 패킷 손실을 유발하지 않는지 확인하십시오.
    - `VoicebotMediaPort`의 `RingBuffer` 사이즈를 조정하여 지연과 안정성 사이의 균형을 맞추십시오.
    - CPU 사용량이 과도할 경우 `SileroVAD`의 `IntraOpNumThreads`를 1로 제한했는지 확인하십시오.

## 3. PBX 연동 관련 (PBX Integration)

### 3.1 등록 실패 (401 Unauthorized)
- **현상**: `PBX Registration Mode Enabled` 로그 이후 `Error creating account` 발생.
- **해결**: 환경 변수 `PBX_USERNAME`과 `PBX_PASSWORD`가 PBX 설정과 일치하는지 확인하십시오.

### 3.2 수신 호 무응답
- **현상**: INVITE 신호가 오지만 게이트웨이가 응답하지 않음.
- **해결**: 게이트웨이가 SIP 포트(기본 5060)에서 Listen 중인지 `netstat -an`으로 확인하고, PBX 시스템에서 게이트웨이의 IP 정보가 올바르게 등록되었는지 점검하십시오.
