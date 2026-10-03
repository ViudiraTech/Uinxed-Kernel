<div align="center">
  <img src="https://github.com/user-attachments/assets/cb3f4ec8-4504-4fe9-b402-8d1588a986a8" height="200" width="200"/>
  <h1 align="center">Uinxed-Kernel</h1>
  <h3 align="center">C 언어로 처음부터 작성된 UNIX 계열 x86-64 커널.</h3>
</div>

<div align="center">
  <img src="https://img.shields.io/badge/License-Apache2.0-blue"/>
  <img src="https://img.shields.io/badge/Language-C-orange"/>
  <img src="https://img.shields.io/badge/Hardware-x64-green"/>
  <img src="https://img.shields.io/badge/Firmware-UEFI/Legacy-yellow"/>
  <a href="https://deepwiki.com/ViudiraTech/Uinxed-Kernel"><img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki"></a>
</div>

<div align="center">

  [English](../README.md) | [中文](README_zh.md) | [日本語](README_ja.md) | **한국어（현재）** | [Русский](README_ru.md) | [Français](README_fr.md)

</div>

---

## 개요

Uinxed는 C 언어로 처음부터 작성된 x86-64용 모놀리식 UNIX 계열 운영체제 커널입니다. [Limine](https://limine-bootloader.org/) 부트로더를 통해 UEFI 및 Legacy 모드에서 부팅하고, SMP(대칭형 멀티프로세싱)로 모든 코어를 기동하며, Linux 호환 시스템 호출 ABI(Linux 6.12 x86-64 번호 매기기, 시스템 호출 0-462)를 구현합니다.

이 프로젝트는 EEVDF 스케줄러, 스왑 지원을 갖춘 통합 페이지 캐시, 다중 파일 시스템을 지원하는 완전한 VFS, Linux 스타일 네트워킹 및 소켓 계층, 그리고 계속 확장되는 장치 드라이버 세트와 같은 현대적인 설계 원칙을 기반으로 실용적이고 자기 완비적인 커널을 구축하는 것을 목표로 합니다. 구현되지 않은 시스템 호출은 `-ENOSYS`를 반환하여 ABI 인터페이스가 성장하는 과정에서 예측 가능성을 유지합니다.

> **현재 상태:** 개발 이미지는 x86-64에서 Alpine Linux 3.23을 부팅할 수 있으며, 작동하는 터미널을 갖춘 Xfce(X11) 데스크톱을 띄울 수 있습니다. PS/2 키보드/마우스 경로, evdev 소비자, poll/epoll 웨이크업, EEVDF 스케줄러는 활발히 검증 중입니다. 이는 여전히 실험적인 커널입니다; GPU, VirtIO, 오디오 및 Linux 호환 ABI의 일부는 아직 불완전할 수 있습니다. Dell PowerEdge R410을 포함한 물리적 서버에서 실행됩니다.

## 핵심 기능

### 스케줄링 및 프로세스 관리

- CPU당 실행 큐와 레드-블랙 트리 타임라인(`vruntime`, `deadline`, `vlag`, `weight`)을 갖춘 EEVDF(Earliest Eligible Virtual Deadline First) 스케줄러
- SMP 지원 태스크 배치, CPU 마이그레이션, 부하 분산, IPI 기반 선점
- 웨이크업 손실을 방지하는 2단계 대기 큐, 스케줄러 타이머 큐로 지원되는 시간 제한 대기
- 견고한 뮤텍스 및 futex 시맨틱스를 위한 우선순위 상속(PI)
- 프로세스당 VMA, 파일 기술자 테이블, 자격 증명을 갖춘 커널 스레드 및 사용자 프로세스
- pids 컨트롤러를 갖춘 Linux 호환 `ptrace` 및 cgroups

### 메모리 관리

- 물리 프레임 할당기(이진 버디)와 4 KiB, 2 MiB, 1 GiB 페이지를 지원하는 표준 4단계 페이징
- 하프 직접 매핑(HHDM) 및 버디 기반 커널 힙/slab 할당기
- 페이지 잠금, LRU 회수, 더티 페이지 라이트백, 리드어헤드, 잘림을 갖춘 통합 페이지 캐시
- 익명 메모리를 위한 스왑 서브시스템: 다중 스왑 영역, 슬롯 할당, 스왑 인/스왑 아웃 폴트 처리

### VFS 및 파일 시스템

- 마운트 지점, inode 유사 노드, 콜백 기반 드라이버 인터페이스를 갖춘 UNIX 스타일 가상 파일 시스템
- 기본 루트 파일 시스템으로 tmpfs; 가상 뷰를 위한 procfs, sysfs, devtmpfs, cpio, cgroupfs
- FAT12/16/32/exFAT(64비트 LBA 및 가변 섹터 크기를 지원하는 FatFS 경유), ext2/ext3/ext4, NTFS(쓰기 지원 포함), ISO 9660(Rock Ridge 포함)

### 네트워킹

- 자체 프로토콜 스택: 이더넷, ARP, IPv4/IPv6, ICMP/ICMPv6, NDP, UDP, TCP
- 이더넷 NIC 드라이버: Intel e1000/e1000e(82540EM, 82545EM, 82546EB, 82541PI, 82574L) 및 Realtek RTL8139/RTL8169, 일반 네트워크 장치 추상화 계층 뒤에 위치
- Linux `AF_INET` / `AF_INET6` 소켓 ABI(`SOCK_DGRAM` / `SOCK_STREAM`), DHCP 클라이언트, `/proc/net` / `/sys/class/net` 뷰

### ABI 및 IPC

- Linux x86-64 시스템 호출 ABI(Linux 6.12 번호 매기기, 0-462)
- `AF_UNIX`, `AF_NETLINK`, `AF_INET`, `AF_INET6` 소켓
- 파이프, `epoll`, `eventfd`, `timerfd`, `signalfd`, `memfd`, `pidfd`, POSIX 메시지 큐, System V IPC
- 우선순위 상속을 갖춘 futex 및 futex2 시스템 호출(`futex_wait` / `futex_wake` / `futex_requeue` / `futex_waitv`); 페이지 캐시로 지원되는 `mmap` / `munmap` / `mremap`
- 파일 시스템 이벤트 알림을 위한 inotify
- Unix98 PTY 및 다중 가상 터미널(VT, 기본 8개)을 포함한 POSIX termios 및 Linux TTY ioctl
- `init_module` / `finit_module` / `delete_module`을 통한 로드 가능 커널 모듈

### 보안 및 추적

- `no_new_privs`, 사용자 알림, seccomp 이벤트 지원을 갖춘 seccomp 필터
- Linux 호환 `ptrace` 검사, 프로세스 수명 주기 이벤트, 시스템 호출 중지 처리

### 드라이버

- **입력:** PS/2 키보드 및 마우스, Linux 호환 `evdev`, USB HID(키보드, 마우스, 소비자 제어)
- **스토리지:** IDE/ATA, AHCI(SATA), NVMe, USB 대용량 저장장치(Bulk-Only Transport / SCSI)
- **오디오:** Sound Blaster 16, Intel HD Audio, ALSA 호환 PCM/제어 ABI
- **디스플레이:** 일반 GPU 드라이버 레지스트리를 갖춘 DRM/KMS 코어(VirtIO-GPU가 내장 드라이버로 제공), 비트맵 폰트를 갖춘 GOP 프레임버퍼 콘솔, 소프트웨어 프레임버퍼 폴백
- **버스:** PCI/PCIe(ECAM + 레거시), USB 호스트 컨트롤러(UHCI/OHCI/EHCI/xHCI), I2C
- **플랫폼:** ACPI, HPET, RTC, 시리얼, IEEE 1284 패러렐 포트, TPM(TIS/CRB, TPM 1.2/2.0)

## 아키텍처

커널은 Limine를 통해 부팅하며, Limine는 `init/main.c`의 `kernel_entry()`에 제어를 넘깁니다. 초기 초기화는 SIMD 상태, 시리얼 출력, 물리 할당기, 페이징, 힙을 기동합니다; 그 다음 플랫폼 계층이 ACPI, TPM, TSC, SMP를 탐색한 후 드라이버와 파일 시스템이 등록됩니다. 프로세스 관리, IPC, 스케줄러는 마지막에 초기화되며, 그 후 부트로더가 제공한 `init` 사용자 공간이 PID 1로 로드되고 스케줄링이 시작됩니다.

```
Limine (UEFI/Legacy)
               |
               v
+------------------------------+     +-------------------------------+
| 초기 초기화                   |---->| 플랫폼 및 드라이버            |
| FPU/SSE -> 시리얼 -> 할당기   |     | ACPI -> SMP -> PCI -> 스토리지 |
| 페이징 -> 힙 -> 모듈          |     | 네트워크 -> 오디오 -> 입력 -> USB |
+------------------------------+     +-------------------------------+
               |                                    |
               v                                    v
+------------------------------+     +-------------------------------+
| VFS 및 파일 시스템            |     | 커널 서비스                   |
| tmpfs/procfs/sysfs -> FAT    |     | 스케줄러 -> 프로세스 -> IPC    |
| ext/NTFS/ISO9660             |     | 시스템 호출 -> 시그널 -> cgroups |
+------------------------------+     +-------------------------------+
               |                                    |
               +-----------------+------------------+
                                 |
                                 v
                   sched_start() -> init (PID 1)
```

## 시작하기

### 전제 조건

- **make**, **gcc**(13.3 이상 권장), **qemu**, **xorriso**
- **clang-format**, **clang-tidy**(포맷팅 및 정적 분석용)
- **kconfig-frontends** + **libncurses-dev**(`menuconfig`용)

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

ArchLinux:

```bash
pacman -Sy make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

### 빌드

```bash
git clone https://github.com/ViudiraTech/Uinxed-Kernel.git
cd Uinxed-Kernel
make
```

이렇게 하면 `UxImage`(커널 이미지)와 `Uinxed-x64.iso`(부팅 가능한 CD 이미지)가 생성됩니다.

### QEMU에서 실행

```bash
make run
```

`make run`은 `-machine q35`, OVMF 펌웨어, `-serial stdio`로 ISO를 부팅하므로 시리얼 출력이 터미널에 표시됩니다.

### 물리적 하드웨어에서 실행

**UEFI 모드**

1. 대상 드라이브를 GPT 파티션 테이블로 변환하고 ESP를 생성합니다.
2. `./assets/Limine`의 내용을 ESP에 복사합니다.
3. `UxImage`를 ESP의 `EFI/Boot/`에 복사합니다.
4. 64비트 UEFI 모드로 부팅합니다(Secure Boot 비활성화).

**Legacy 모드**

1. `Uinxed-x64.iso`를 드라이브에 굽습니다.
2. 64비트 머신에서 그것으로 부팅합니다.

두 모드 모두 [Ventoy](https://www.ventoy.net/)를 통해서도 작동합니다: ISO를 드라이브에 복사하고 부팅 메뉴에서 선택하기만 하면 됩니다.

## 프로젝트 레이아웃

```
Uinxed-Kernel/
|-- assets/           # 빌드 및 부팅 리소스
|-- boot/             # 부트 프로토콜 구조체 및 인터페이스
|-- docs/             # 프로젝트 문서 및 기술 노트
|-- drivers/          # 하드웨어 드라이버 및 장치 지원
|-- fs/               # 파일 시스템 구현 및 VFS 컴포넌트
|-- include/          # 커널 헤더 및 공용 인터페이스
|-- init/             # 커널 진입점 및 초기화 루틴
|-- ipc/              # 프로세스 간 통신 메커니즘
|-- kernel/           # 핵심 커널 서브시스템 및 런타임 서비스
|-- libs/             # 내부 커널 라이브러리 및 유틸리티 함수
|-- mem/              # 메모리 관리 서브시스템
|-- net/              # 네트워크 스택 및 프로토콜 구현
|-- scripts/          # 빌드, 설정, 유지보수 스크립트
|-- security/         # 보안 및 정책 관련 컴포넌트
|-- tools/            # 개발, 디버깅, 보조 도구
|-- .clang-format     # 코드 포맷팅 설정
|-- .clang-tidy       # 정적 분석 설정
|-- .config-default   # 기본 커널 설정
|-- .gitignore        # Git 무시 규칙
|-- CONTRIBUTING.md   # 기여 가이드
|-- CREDITS           # 기여자 목록
|-- Kconfig           # 커널 설정 시스템
|-- LICENSE           # 프로젝트 라이선스(Apache License 2.0)
|-- Makefile          # 빌드 시스템
|-- NOTICE            # 타사 라이선스 및 귀속 고지
|-- README.md         # 프로젝트 개요 및 소개
`-- SECURITY.md       # 보안 정책 및 취약점 보고
```

## FAQ

**어떤 개발 명령을 사용할 수 있나요?**

`make help`를 실행하여 지원되는 모든 대상(format, check, menuconfig, gen.clangd 등)을 나열하세요.

**편집기에서 "XXX.h file not found" 오류가 발생하나요?**

clangd를 LSP 서버로 사용하는 경우 다음으로 프로젝트 설정을 생성하세요:

```bash
make gen.clangd
```

다른 LSP 서버의 경우 Makefile에서 설정을 조정하세요.

**커널 로그를 어떻게 읽나요?**

로그 출력은 부트 콘솔로 전송됩니다. 기본적으로 화면(`tty0`, `assets/Limine/Limine/limine.conf`의 `kernel_cmdline: console=tty0`로 설정)으로 전송됩니다. 시리얼 포트를 통해 로그를 캡처하려면:

1. `limine.conf`의 `kernel_cmdline`을 `console=ttyS0`(또는 `ttyS1`-`ttyS3`)로 변경합니다.
2. 재빌드하고 실행합니다. `make run`은 이미 QEMU의 시리얼 출력을 터미널에 연결합니다(`-serial stdio`).

`console=` 매개변수는 `tty0`(VGA 화면)과 `ttyS0`-`ttyS3`(시리얼 포트)를 허용합니다. 화면 콘솔은 출력을 버퍼링하며 VGA 큐가 오버플로우되거나 커널이 부팅 중에 행되면 데이터를 드롭할 수 있습니다. 시리얼 콘솔이 행을 디버깅하는 신뢰할 수 있는 방법입니다. `plogk` 디버그 메시지는 `CONFIG_KERNEL_LOG`가 활성화된 경우에만 컴파일됩니다.

**모든 Linux 시스템 호출이 구현되었나요?**

아니요. 시스템 호출 테이블은 Linux 6.12 x86-64 번호 매기기(시스템 호출 0-462)를 따르지만 일부만 구현되었습니다. 구현되지 않은 시스템 호출은 크래시 대신 `-ENOSYS`를 반환하며, 구현 세트는 프로젝트가 발전함에 따라 성장합니다.

**예상했던 드라이버가 작동하지 않는 이유는 무엇인가요(예: VirtIO-GPU, SB16)?**

일부 서브시스템은 Kconfig에서 기본적으로 비활성화되어 있습니다. 예를 들어 `VIRTIO`와 `VIRTIO_GPU`는 기본값이 `n`이고, `SOUND_SB16`은 기본값이 `n`입니다. `make menuconfig`로 활성화하고(kconfig-frontends와 libncurses-dev 필요), 해당 장치가 VM 또는 하드웨어에 있는지 확인하세요. GPU 드라이버는 `drivers/gpu/gpu_drivers.c`의 레지스트리를 통해 검색됩니다; 새 GPU 드라이버를 추가하려면 거기에 항목을 하나 추가하면 됩니다. 빌드는 `.config`가 있으면 읽고, 없으면 `.config-default`를 읽습니다; 생성된 `.config`가 우선합니다.

**실제 하드웨어에서 실행할 수 있나요?**

네, 하지만 실험적인 커널로 취급하세요. 위의 물리적 하드웨어 단계를 따르고, 일회용 머신이나 테스트 디스크를 선호하세요 — 파일 시스템 드라이버(특히 NTFS 라이터)는 아직 중요한 데이터에 안전하지 않습니다.

## 라이선스 및 면책 조항

### 라이선스

이 프로젝트는 [Apache License 2.0](../LICENSE) 하에 라이선스가 부여됩니다.
이 배포판에는 각자의 라이선스 하에 타사 소프트웨어가 포함되어 있습니다. 전체 귀속 및 라이선스 텍스트는 [NOTICE](../NOTICE)에 수집되어 있습니다.
코드베이스의 일부는 상호 운용성을 위해 Linux 커널 인터페이스를 참조하고 재구현합니다. 이들은 공개 인터페이스 및 사양의 독립적인 구현이며 Linux 커널 소스 코드를 포함하지 않습니다.

### 면책 조항

Uinxed는 활발히 개발 중인 실험적인 커널입니다. 상품성 또는 특정 목적에의 적합성에 대한 보증을 포함하되 이에 국한되지 않는 어떠한 종류의 명시적 또는 묵시적 보증 없이 "있는 그대로" 제공됩니다.

- 커널과 그 파일 시스템 드라이버(NTFS 라이터 포함)는 프로덕션 데이터에 **안전하지 않습니다**. 일회용 디스크나 가상 머신에서만 사용하세요.
- 하드웨어 지원은 불완전합니다; 테스트되지 않은 실제 하드웨어에서 실행하면 행, 크래시 또는 데이터 손실이 발생할 수 있습니다.
- 이 프로젝트는 Linux, Limine 또는 언급된 오픈소스 프로젝트와 제휴되어 있지 않습니다. 모든 상표는 각 소유자에게 있습니다.

이 소프트웨어를 사용함으로써 사용자는 자신의 위험 부담으로 사용한다는 것을 인정합니다.

## 연락처

- 이메일: rainy101112@163.com | 2609948707@qq.com | 3585302907@qq.com
- Discord: [서버 참여](https://discord.gg/nTkg7HCpy7)
- QQ 그룹: [983673299](https://qm.qq.com/q/8goacFf1iU)
