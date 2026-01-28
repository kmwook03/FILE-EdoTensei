# FILE-EdoTensei

### 개요
본 프로젝트는 NTFS 등의 파일 시스템에서 메타데이터(MFT 영역 등)가 손상되거나 의도적으로 파괴되어 OS가 파일을 식별할 수 없는 상황을 해결하기 위한 디지털 포렌식 도구입니다. 파일 시스템의 논리적 구조에 의존하지 않고, 저장 매체의 **Raw Data(바이트 스트림)**를 직접 스캔하여 파일 고유의 시그니처를 기반으로 데이터를 재구성합니다

### 핵심 기능
> Disk I/O

표준 I/O 라이브러리 대신 리눅스 시스템 콜을 직접 호출하여 디스크 I/O를 정밀하게 제어하고 오버헤드를 최소화합니다.

> 고속 패턴 매칭

Boyer-Moore-Horspool(BMH) 알고리즘을 채택하여 대용량 디스크 이미지 분석 시 단순 선형 탐색 대비 효율적인 탐색을 수행합니다.

> 지원 포맷

- JPG
- PNG
- PDF

### 알고리즘 & 로직

본 도구는 유한 상태 기계(Finite State Machine) 모델을 기반으로 동작합니다.

1. SEARCHING: 버퍼 내에서 타겟 파일의 헤더 시그니처를 스캔합니다.
2. EXTRACTING: 헤더 발견 시 파일 쓰기를 시작하며, 다음 이벤트를 감시합니다.
    
    - 푸터 발견: 유효한 종료 지점을 식별하여 파일 저장을 완료하고 다시 SEARCHING 상태로 전이합니다.
    - 헤더 충돌: 새로운 파일 헤더가 발견되면 현재 파일 추출을 강제 종료하고 새 파일을 생성합니다.

### 빌드 & 실행

> 환경 요구 사항

- OS: Linux(Ubuntu 24.04 LTS 권장)
- Compiler: g++(C++17 지원 필수)
- Build Tool: CMake 3.10 이상

> 빌드 방법

```bash
# 1. 리포지토리 복제
git clone https://github.com/kmwook03/FILE-EdoTensei.git
cd FILE-EdoTensei

# 2. 빌드 디렉토리 생성 및 이동
mkdir build && cd build

# 3. CMake 설정 및 컴파일 수행
cmake ..
make
```

> 실행 방법

```bash
# 실제 연결된 물리 디스크로 설정해주세요.
# 예) /dev/sde
sudo ./app/FILEEdo /dev/sde
```
