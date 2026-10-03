<div align="center">
  <img src="https://github.com/user-attachments/assets/cb3f4ec8-4504-4fe9-b402-8d1588a986a8" height="200" width="200"/>
  <h1 align="center">Uinxed-Kernel</h1>
  <h3 align="center">C言語でゼロから書かれたUNIXライクなx86-64カーネル。</h3>
</div>

<div align="center">
  <img src="https://img.shields.io/badge/License-Apache2.0-blue"/>
  <img src="https://img.shields.io/badge/Language-C-orange"/>
  <img src="https://img.shields.io/badge/Hardware-x64-green"/>
  <img src="https://img.shields.io/badge/Firmware-UEFI/Legacy-yellow"/>
  <a href="https://deepwiki.com/ViudiraTech/Uinxed-Kernel"><img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki"></a>
</div>

<div align="center">

  [English](../README.md) | [中文](README_zh.md) | **日本語（現在）** | [한국어](README_ko.md) | [Русский](README_ru.md) | [Français](README_fr.md)

</div>

---

## 概要

Uinxedは、C言語でゼロから書かれたx86-64向けのモノリシックでUNIXライクなオペレーティングシステムカーネルです。[Limine](https://limine-bootloader.org/)ブートローダーを介してUEFIモードとLegacyモードの両方で起動し、SMP（対称型マルチプロセッシング）により全コアを起動し、Linux互換のシステムコールABI（Linux 6.12 x86-64番号付け、システムコール0-462）を実装しています。

このプロジェクトは、EEVDFスケジューラ、スワップ対応の統一ページキャッシュ、複数ファイルシステムをサポートする完全なVFS、Linuxスタイルのネットワークおよびソケットレイヤー、そして成長し続けるデバイスドライバー群といった現代的な設計原則に基づき、実用的で自己完結したカーネルを構築することを目指しています。未実装のシステムコールは `-ENOSYS` を返し、ABIインターフェースが成長する過程で予測可能な状態を維持します。

> **現在の状況：** 開発用イメージはx86-64上でAlpine Linux 3.23を起動でき、動作するターミナルを備えたXfce（X11）デスクトップを起動できます。PS/2キーボード/マウスパス、evdevコンシューマー、poll/epollウェイクアップ、EEVDFスケジューラは活発に検証中です。これは依然として実験的なカーネルであり、GPU、VirtIO、オーディオ、およびLinux互換ABIの一部はまだ不完全な可能性があります。Dell PowerEdge R410を含む物理サーバーで動作します。

## 主な機能

### スケジューリングとプロセス管理

- EEVDF（Earliest Eligible Virtual Deadline First）スケジューラ。CPUごとの実行キューと赤黒木タイムライン（`vruntime`、`deadline`、`vlag`、`weight`）を備える
- SMP対応のタスク配置、CPUマイグレーション、負荷分散、IPIベースのプリエンプション
- ウェイクアップの喪失を防ぐ2フェーズ待ちキュー、およびスケジューラタイマーキューによってサポートされるタイムアウト付き待機
- 堅牢なミューテックスとfutexセマンティクスのための優先度継承（PI）
- プロセスごとのVMA、ファイル記述子テーブル、資格情報を備えたカーネルスレッドとユーザープロセス
- Linux互換の `ptrace` とpidsコントローラーを備えたcgroups

### メモリ管理

- 物理フレームアロケータ（バイナリバディ）と、4 KiB、2 MiB、1 GiBページをサポートする標準4レベルページング
- ハーフダイレクトマップ（HHDM）とバディベースのカーネルヒープ/slabアロケータ
- ページロック、LRUリクレイム、ダーティページライトバック、リードアヘッド、切り捨てを備えた統一ページキャッシュ
- 匿名メモリ用のスワップサブシステム：複数スワップ領域、スロット割り当て、スワップイン/スワップアウトフォールト処理

### VFSとファイルシステム

- マウントポイント、inodeライクノード、コールバックベースのドライバーインターフェースを備えたUNIXスタイルの仮想ファイルシステム
- デフォルトのルートファイルシステムとしてtmpfs。仮想ビュー用にprocfs、sysfs、devtmpfs、cpio、cgroupfs
- FAT12/16/32/exFAT（64ビットLBAと可変セクターサイズをサポートするFatFS経由）、ext2/ext3/ext4、NTFS（書き込みサポート付き）、ISO 9660（Rock Ridge付き）

### ネットワーク

- 独自開発のプロトコルスタック：イーサネット、ARP、IPv4/IPv6、ICMP/ICMPv6、NDP、UDP、TCP
- イーサネットNICドライバー：Intel e1000/e1000e（82540EM、82545EM、82546EB、82541PI、82574L）とRealtek RTL8139/RTL8169。汎用ネットワークデバイス抽象化の背後にある
- Linux `AF_INET` / `AF_INET6` ソケットABI（`SOCK_DGRAM` / `SOCK_STREAM`）、DHCPクライアント、`/proc/net` / `/sys/class/net` ビュー

### ABIとプロセス間通信

- Linux x86-64システムコールABI（Linux 6.12番号付け、0-462）
- `AF_UNIX`、`AF_NETLINK`、`AF_INET`、`AF_INET6` ソケット
- パイプ、`epoll`、`eventfd`、`timerfd`、`signalfd`、`memfd`、`pidfd`、POSIXメッセージキュー、System V IPC
- 優先度継承付きfutexとfutex2システムコール（`futex_wait` / `futex_wake` / `futex_requeue` / `futex_waitv`）。ページキャッシュによってサポートされる `mmap` / `munmap` / `mremap`
- ファイルシステムイベント通知用のinotify
- POSIX termiosとLinux TTY ioctl。Unix98 PTYと複数の仮想ターミナル（VT、デフォルト8）を含む
- `init_module` / `finit_module` / `delete_module` によるローダブルカーネルモジュール

### セキュリティとトレース

- `no_new_privs`、ユーザー通知、seccompイベントサポートを備えたseccompフィルター
- Linux互換の `ptrace` 検査、プロセスライフサイクルイベント、システムコール停止処理

### ドライバー

- **入力：** PS/2キーボードとマウス、Linux互換の `evdev`、USB HID（キーボード、マウス、コンシューマーコントロール）
- **ストレージ：** IDE/ATA、AHCI（SATA）、NVMe、USBマスストレージ（Bulk-Only Transport / SCSI）
- **オーディオ：** Sound Blaster 16、Intel HD Audio、ALSA互換PCM/コントロールABI
- **ディスプレイ：** 汎用GPUドライバーレジストリを備えたDRM/KMSコア（VirtIO-GPUが組み込みドライバーとして同梱）、ビットマップフォント付きGOPフレームバッファコンソール、ソフトウェアフレームバッファフォールバック
- **バス：** PCI/PCIe（ECAM + レガシー）、USBホストコントローラー（UHCI/OHCI/EHCI/xHCI）、I2C
- **プラットフォーム：** ACPI、HPET、RTC、シリアル、IEEE 1284パラレルポート、TPM（TIS/CRB、TPM 1.2/2.0）

## アーキテクチャ

カーネルはLimineを介して起動し、Limineは `init/main.c` の `kernel_entry()` に制御を引き渡します。初期初期化ではSIMD状態、シリアル出力、物理アロケータ、ページング、ヒープを起動します。次にプラットフォームレイヤーがACPI、TPM、TSC、SMPをプローブした後、ドライバーとファイルシステムが登録されます。プロセス管理、IPC、スケジューラは最後に初期化され、その後ブートローダーが提供する `init` ユーザー空間がPID 1としてロードされ、スケジューリングが開始されます。

```
Limine (UEFI/Legacy)
               |
               v
+------------------------------+     +-------------------------------+
| 初期初期化                   |---->| プラットフォームとドライバー    |
| FPU/SSE -> シリアル -> アロケータ |   | ACPI -> SMP -> PCI -> ストレージ |
| ページング -> ヒープ -> モジュール | | ネット -> オーディオ -> 入力 -> USB |
+------------------------------+     +-------------------------------+
               |                                    |
               v                                    v
+------------------------------+     +-------------------------------+
| VFSとファイルシステム          |     | カーネルサービス               |
| tmpfs/procfs/sysfs -> FAT    |     | スケジューラ -> プロセス -> IPC |
| ext/NTFS/ISO9660             |     | システムコール -> シグナル -> cgroups |
+------------------------------+     +-------------------------------+
               |                                    |
               +-----------------+------------------+
                                 |
                                 v
                   sched_start() -> init (PID 1)
```

## はじめに

### 前提条件

- **make**、**gcc**（13.3以上推奨）、**qemu**、**xorriso**
- **clang-format**、**clang-tidy**（フォーマットと静的解析用）
- **kconfig-frontends** + **libncurses-dev**（`menuconfig` 用）

Debian/Ubuntu：

```bash
sudo apt update
sudo apt install make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

ArchLinux：

```bash
pacman -Sy make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

### ビルド

```bash
git clone https://github.com/ViudiraTech/Uinxed-Kernel.git
cd Uinxed-Kernel
make
```

これにより `UxImage`（カーネルイメージ）と `Uinxed-x64.iso`（起動可能なCDイメージ）が生成されます。

### QEMUで実行

```bash
make run
```

`make run` は `-machine q35`、OVMFファームウェア、`-serial stdio` でISOを起動するため、シリアル出力がターミナルに表示されます。

### 物理ハードウェアで実行

**UEFIモード**

1. ターゲットドライブをGPTパーティションテーブルに変換し、ESPを作成します。
2. `./assets/Limine` の内容をESPにコピーします。
3. `UxImage` をESPの `EFI/Boot/` にコピーします。
4. 64ビットUEFIモードで起動します（Secure Bootは無効化）。

**Legacyモード**

1. `Uinxed-x64.iso` をドライブに書き込みます。
2. 64ビットマシンでそこから起動します。

両モードとも [Ventoy](https://www.ventoy.net/) 経由でも起動できます。ISOをドライブにコピーし、ブートメニューから選択するだけです。

## プロジェクト構成

```
Uinxed-Kernel/
|-- assets/           # ビルドとブートのリソース
|-- boot/             # ブートプロトコル構造体とインターフェース
|-- docs/             # プロジェクトドキュメントと技術ノート
|-- drivers/          # ハードウェアドライバーとデバイスサポート
|-- fs/               # ファイルシステム実装とVFSコンポーネント
|-- include/          # カーネルヘッダーとパブリックインターフェース
|-- init/             # カーネルエントリと初期化ルーチン
|-- ipc/              # プロセス間通信メカニズム
|-- kernel/           # コアカーネルサブシステムとランタイムサービス
|-- libs/             # 内部カーネルライブラリとユーティリティ関数
|-- mem/              # メモリ管理サブシステム
|-- net/              # ネットワークスタックとプロトコル実装
|-- scripts/          # ビルド、設定、メンテナンススクリプト
|-- security/         # セキュリティとポリシー関連コンポーネント
|-- tools/            # 開発、デバッグ、補助ツール
|-- .clang-format     # コードフォーマット設定
|-- .clang-tidy       # 静的解析設定
|-- .config-default   # デフォルトカーネル設定
|-- .gitignore        # Git無視ルール
|-- CONTRIBUTING.md   # コントリビューションガイド
|-- CREDITS           # コントリビューター一覧
|-- Kconfig           # カーネル設定システム
|-- LICENSE           # プロジェクトライセンス（Apache License 2.0）
|-- Makefile          # ビルドシステム
|-- NOTICE            # サードパーティライセンスと帰属表示
|-- README.md         # プロジェクト概要と紹介
`-- SECURITY.md       # セキュリティポリシーと脆弱性報告
```

## よくある質問

**どのような開発コマンドが利用できますか？**

`make help` を実行して、サポートされているすべてのターゲット（format、check、menuconfig、gen.clangdなど）をリストアップしてください。

**エディターで "XXX.h file not found" エラーが出ますか？**

clangdをLSPサーバーとして使用している場合は、以下でプロジェクト設定を生成してください：

```bash
make gen.clangd
```

他のLSPサーバーについては、Makefileから設定を調整してください。

**カーネルログを読むにはどうしますか？**

ログ出力はブートコンソールに送信されます。デフォルトでは画面（`tty0`。`assets/Limine/Limine/limine.conf` の `kernel_cmdline: console=tty0` で設定）に送信されます。シリアルポート経由でログをキャプチャするには：

1. `limine.conf` の `kernel_cmdline` を `console=ttyS0`（または `ttyS1`-`ttyS3`）に変更します。
2. 再ビルドして実行します。`make run` はすでにQEMUのシリアル出力をターミナルに接続しています（`-serial stdio`）。

`console=` パラメータは `tty0`（VGA画面）と `ttyS0`-`ttyS3`（シリアルポート）を受け入れます。画面コンソールは出力をバッファリングし、VGAキューがオーバーフローしたりカーネルがブート途中でハングしたりするとデータをドロップする可能性があることに注意してください。シリアルコンソールはハングをデバッグする信頼できる方法です。`plogk` デバッグメッセージは `CONFIG_KERNEL_LOG` が有効な場合にのみコンパイルされます。

**すべてのLinuxシステムコールが実装されていますか？**

いいえ。システムコールテーブルはLinux 6.12 x86-64番号付け（システムコール0-462）に従っていますが、サブセットのみが実装されています。未実装のシステムコールはクラッシュする代わりに `-ENOSYS` を返し、実装セットはプロジェクトの発展に伴って成長します。

**期待していたドライバーが動作しないのはなぜですか（例：VirtIO-GPU、SB16）？**

一部のサブシステムはKconfigでデフォルトで無効になっています。例えば、`VIRTIO` と `VIRTIO_GPU` はデフォルトで `n`、`SOUND_SB16` はデフォルトで `n` です。`make menuconfig` で有効にし（kconfig-frontendsとlibncurses-devが必要）、対応するデバイスがVMまたはハードウェアに存在することを確認してください。GPUドライバーは `drivers/gpu/gpu_drivers.c` のレジストリを介して検出されます。新しいGPUドライバーを追加するには、そこにエントリを1つ追加します。ビルドは `.config` が存在する場合はそれを読み取り、存在しない場合は `.config-default` を読み取ります。生成された `.config` が優先されます。

**実際のハードウェアで実行できますか？**

はい、ただし実験的なカーネルとして扱ってください。上記の物理ハードウェア手順に従い、使い捨てのマシンまたはテスト用ディスクを優先してください——ファイルシステムドライバー（特にNTFSライター）はまだ重要なデータに対して安全ではありません。

## ライセンスと免責事項

### ライセンス

このプロジェクトは [Apache License 2.0](../LICENSE) の下でライセンスされています。
このディストリビューションには、それぞれ独自のライセンスの下でサードパーティソフトウェアが含まれています。完全な帰属とライセンステキストは [NOTICE](../NOTICE) に収集されています。
コードベースの一部は、相互運用性のためにLinuxカーネルインターフェースを参照および再実装しています。これらはパブリックインターフェースと仕様の独立した実装であり、Linuxカーネルソースコードを含んでいません。

### 免責事項

Uinxedは活発に開発中の実験的なカーネルです。商品性または特定目的への適合性の保証を含むがこれらに限定されない、いかなる種類の明示または黙示の保証もなく、「現状のまま」提供されます。

- カーネルとそのファイルシステムドライバー（NTFSライターを含む）は、本番データに対して**安全ではありません**。使い捨てのディスクまたは仮想マシンでのみ使用してください。
- ハードウェアサポートは不完全です。テストされていない実ハードウェアで実行すると、ハング、クラッシュ、データ損失が発生する可能性があります。
- このプロジェクトは、Linux、Limine、または言及されているオープンソースプロジェクトとは提携していません。すべての商標はそれぞれの所有者に帰属します。

このソフトウェアを使用することにより、自己責任で使用することを承認したものとみなされます。

## 連絡先

- メール：rainy101112@163.com | 2609948707@qq.com | 3585302907@qq.com
- Discord：[サーバーに参加](https://discord.gg/nTkg7HCpy7)
- QQグループ：[983673299](https://qm.qq.com/q/8goacFf1iU)
