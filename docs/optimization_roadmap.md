# Voicebot Gateway (VBGW) 초저지연 성능 최적화 로드맵

이 문서는 VBGW가 100콜 이상의 단일 노드 성능을 넘어, **1,000콜 이상의 초거대 트래픽**을 초저지연(Low-Latency) 수준으로 수용하기 위한 기술적 고도화 및 최적화 로드맵을 정의합니다. 현재 프로덕션 준비(Day-2)가 완료된 상태를 기점으로, 단계별로 적용 가능한 최적화 기법을 제안합니다.

---

## 단계별 최적화 로드맵

### Phase 1: 아키텍처 비동기화 및 병목 제거 (High-Concurrency)

현재 구조에서 가장 먼저 CPU 병목이 발생할 수 있는 스레드 경합(Thread Contention)을 제거하는 단계입니다.

1. **VAD 추론의 비동기 오프로드 (Asynchronous Offloading)**
   - **현재**: PJSIP RTP 워커 스레드 내부 수신 콜백에서 ONNX VAD 인퍼런스와 gRPC 네트워크 전송이 직렬 동기식으로 블로킹됩니다.
   - **개선안**: Lock-free Ring Buffer(또는 MPMC Queue)를 도입하여, RTP 워커는 메모리에 오디오 바이트만 밀어 넣고 즉시 복귀(Return)하도록 분리합니다. 별도로 뒷단에 구성된 C++ 워커 스레드 풀(AI Worker Pool)이 큐를 Polling 하며 VAD와 gRPC 전송을 비동기 전담 처리합니다.
   
2. **CPU Core Affinity (Pinning)**
   - **도입**: 컨텍스트 스위칭으로 인한 L2/L3 캐시 미스를 방지하기 위해, 오디오 전용 스레드와 VAD 연산 스레드를 각각의 물리 CPU Core에 강제 할당(`pthread_setaffinity_np`)하는 Numa-Aware 아키텍처를 도입합니다.

---

### Phase 2: 메모리 할당 지연 최적화 (Zero-Allocation)

C++ 환경에서 빈번한 객체 생성/소멸은 가비지 컬렉터가 없더라도 힙(Heap) 메모리 파편화를 유발하고 지연시간의 스파이크(Jitter)를 발생시킵니다.

1. **Session & Media Object Pool 도입**
   - **현재**: 콜이 들어올 때마다 `new VoicebotCall()`, `new VoicebotMediaPort()`를 호출합니다.
   - **개선안**: VBGW 부팅 지점에 10,000개의 세션 객체와 RTP 버퍼를 미리 할당(`std::vector` 기반 Slab Allocator)합니다. 호가 인입되면 초기화만 진행하여 바로 재사용(Reuse)하는 Object Pooling 패턴을 도입해 동적 할당의 런타임 오버헤드를 0으로 만듭니다.

2. **Ring Buffer 캐리지 최소화 (Zero-Copy)**
   - STT로 보내고 받는 gRPC 바이트 스트림과 PJSIP 프레임 패킷 사이의 복사(Copy)를 1회 이하로 줄이는 제로 카피 버퍼 릴레이 구조를 도입합니다.

---

### Phase 3: 네트워크 페이로드 최적화 (Network & Payload)

대규모 콜센터 환경에서는 1,000콜의 오디오만 전송해도 엄청난 네트워크 트래픽 파이프가 낭비됩니다. 외부 AI 엔진으로 향하는 I/O를 최소화해야 합니다.

1. **무음 구간(Silence) 네트워크 페이로드 차단 (VAD Gating)**
   - **도입**: 현재는 수신되는 20ms마다 gRPC 스트림으로 무조건 쏘고 있습니다. VAD가 `false`로 떨어져 명백한 무음(또는 단순 배경 소음)으로 판정된 구간이 300ms 이상 지속될 경우, 아예 AI 봇 백엔드로 Audio Chunk를 보내지 않도록 차단(Dropping)합니다. 이를 통해 **네트워크 트래픽 및 AI 디코딩 리소스를 60% 이상 절감**할 수 있습니다.

2. **gRPC Channel Multiplexing 및 커넥션 풀링**
   - 여러 발신자의 gRPC Stream이 단일 gRPC Channel(HTTP/2 커넥션) 하나에 의존하면 HTTP/2 Head-of-Line Blocking 한계에 부딪힐 수 있습니다. Destination 타겟별로 여러 개의 Channel을 Round-Robin시키는 채널 풀링을 구현합니다.

---

### Phase 4: 대형 PBX 장비 호환성 향상 (Enterprise VoIP)

표준 통신 규격을 준수하여 모든 통신사 및 Enterprise SBC와의 연동에서 발생할 수 있는 변수들을 제어합니다.

1. **Codec Passthrough 및 협상 고도화**
   - **도입**: Opus 우대를 넘어, PBX가 PCMU(G.711u) 전용으로 협상을 걸어왔을 때 SpeexDSP 리샘플링 비용을 줄이기 위해 G.711 데이터를 그대로 PCM으로 풀거나, AI 자체에서 G.711을 리샘플링 없이 수용할 수 있도록 데이터 패스를 이원화합니다.
   
2. **SIP Keep-Alive (SIP OPTIONS) 핑퐁 처리**
   - VBGW와 PBX 간의 세션 단절 여부를 실시간으로 상호 감지하기 위해, 통신사가 주로 요구하는 `SIP OPTIONS` 핑에 대한 200 OK 하트비트 응답기능을 독립 컴포넌트로 정밀 대응합니다.

---

### 요약 및 제안 일정 (Action Item)

- **[단기]** `Phase 1 (VAD 비동기 오프로딩)` 및 `Phase 3 (무음 구간 Gating 차단)`은 가장 적은 공수로 극단적인 체감 레이턴시 향상과 성능 개선을 가져오는 **High-Value 아이템**입니다. 다음 버그픽스나 메이저 배포 스케줄 내에 우선적으로 구현하는 것을 권장합니다.
- **[중장기]** `Phase 2 (객체 풀링)` 및 `Phase 4`는 일 최고 처리량이 수십만 건 이상의 초대형 프로덕션에 도입될 무렵 모니터링 수치(5xx 에러율이나 CPU 병목 여부)를 기반으로 단계적 적용을 권장합니다.
