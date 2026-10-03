<div align="center">
  <img src="https://github.com/user-attachments/assets/cb3f4ec8-4504-4fe9-b402-8d1588a986a8" height="200" width="200"/>
  <h1 align="center">Uinxed-Kernel</h1>
  <h3 align="center">Un noyau x86-64 de type UNIX écrit de zéro en C.</h3>
</div>

<div align="center">
  <img src="https://img.shields.io/badge/License-Apache2.0-blue"/>
  <img src="https://img.shields.io/badge/Language-C-orange"/>
  <img src="https://img.shields.io/badge/Hardware-x64-green"/>
  <img src="https://img.shields.io/badge/Firmware-UEFI/Legacy-yellow"/>
  <a href="https://deepwiki.com/ViudiraTech/Uinxed-Kernel"><img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki"></a>
</div>

<div align="center">

  [English](../README.md) | [中文](README_zh.md) | [日本語](README_ja.md) | [한국어](README_ko.md) | [Русский](README_ru.md) | **Français（actuel）**

</div>

---

## Aperçu

Uinxed est un noyau de système d'exploitation monolithique de type UNIX pour x86-64, entièrement écrit de zéro en C. Il démarre via le chargeur de démarrage [Limine](https://limine-bootloader.org/) en modes UEFI et Legacy, lance tous les cœurs via le SMP (traitement multiprocesseur symétrique) et implémente une ABI d'appels système compatible Linux (numérotation Linux 6.12 x86-64, appels système 0-462).

Le projet vise à construire un noyau pratique et autonome avec des principes de conception modernes : un ordonnanceur EEVDF, un cache de pages unifié avec prise en charge du swap, un VFS complet prenant en charge plusieurs systèmes de fichiers, une couche réseau et de sockets de style Linux, et un ensemble croissant de pilotes de périphériques. Les appels système non implémentés renvoient `-ENOSYS`, gardant l'interface ABI prévisible au fur et à mesure de sa croissance.

> **État actuel :** L'image de développement démarre Alpine Linux 3.23 sur x86-64 et peut lancer un bureau Xfce (X11) avec un terminal fonctionnel. Les chemins clavier/souris PS/2, les consommateurs evdev, les réveils poll/epoll et l'ordonnanceur EEVDF sont en cours de validation active. C'est toujours un noyau expérimental ; le GPU, VirtIO, l'audio et certaines parties de l'ABI compatible Linux peuvent encore être incomplets. Fonctionne sur des serveurs physiques, dont le Dell PowerEdge R410.

## Fonctionnalités principales

### Ordonnancement et gestion des processus

- Ordonnanceur EEVDF (Earliest Eligible Virtual Deadline First) avec des files d'attente d'exécution par CPU et une chronologie sur arbre rouge-noir (`vruntime`, `deadline`, `vlag`, `weight`)
- Placement des tâches compatible SMP, migration CPU, équilibrage de charge et préemption basée sur IPI
- Files d'attente à deux phases évitant les réveils perdus, et attente avec délai soutenue par la file d'attente des minuteurs de l'ordonnanceur
- Héritage de priorité (PI) pour une sémantique robuste des mutex et futex
- Threads noyau et processus utilisateur avec VMA par processus, tables de descripteurs de fichiers et informations d'identification
- `ptrace` compatible Linux et cgroups avec contrôleur pids

### Gestion de la mémoire

- Allocateur de cadres physiques (buddy binaire) et pagination standard à 4 niveaux prenant en charge les pages 4 Kio, 2 Mio et 1 Gio
- Cartographie directe de la moitié haute (`HHDM`) et tas noyau/allocateur slab basés sur buddy
- Cache de pages unifié avec verrouillage de pages, éviction LRU, écriture différée des pages sales, lecture anticipée et troncature
- Sous-système de swap pour la mémoire anonyme : plusieurs zones de swap, allocation d'emplacements et gestion des fautes swap-in/swap-out

### VFS et systèmes de fichiers

- Système de fichiers virtuel de style UNIX avec points de montage, nœuds de type inode et interface de pilote basée sur des rappels
- tmpfs comme système de fichiers racine par défaut ; procfs, sysfs, devtmpfs, cpio et cgroupfs pour les vues virtuelles
- FAT12/16/32/exFAT (via FatFS avec LBA 64 bits et taille de secteur variable), ext2/ext3/ext4, NTFS (avec prise en charge en écriture) et ISO 9660 (avec Rock Ridge)

### Réseau

- Pile de protocoles maison : Ethernet, ARP, IPv4/IPv6, ICMP/ICMPv6, NDP, UDP et TCP
- Pilotes de cartes réseau Ethernet : Intel e1000/e1000e (82540EM, 82545EM, 82546EB, 82541PI, 82574L) et Realtek RTL8139/RTL8169, derrière une abstraction de périphérique réseau générique
- ABI de sockets Linux `AF_INET` / `AF_INET6` (`SOCK_DGRAM` / `SOCK_STREAM`), client DHCP et vues `/proc/net` / `/sys/class/net`

### ABI et communication inter-processus

- ABI d'appels système Linux x86-64 (numérotation Linux 6.12, 0-462)
- Sockets `AF_UNIX`, `AF_NETLINK`, `AF_INET`, `AF_INET6`
- Tubes, `epoll`, `eventfd`, `timerfd`, `signalfd`, `memfd`, `pidfd`, files de messages POSIX et IPC System V
- futex avec héritage de priorité et appels système futex2 (`futex_wait` / `futex_wake` / `futex_requeue` / `futex_waitv`) ; `mmap` / `munmap` / `mremap` soutenus par le cache de pages
- inotify pour les notifications d'événements du système de fichiers
- termios POSIX et ioctl TTY Linux, y compris PTY Unix98 et plusieurs terminaux virtuels (VT, 8 par défaut)
- Modules noyau chargeables via `init_module` / `finit_module` / `delete_module`

### Sécurité et traçage

- Filtres seccomp avec `no_new_privs`, notifications utilisateur et prise en charge des événements seccomp
- Inspection `ptrace` compatible Linux, événements du cycle de vie des processus et gestion de l'arrêt des appels système

### Pilotes

- **Entrée :** Clavier et souris PS/2, `evdev` compatible Linux, USB HID (clavier, souris, contrôle consommateur)
- **Stockage :** IDE/ATA, AHCI (SATA), NVMe et stockage de masse USB (Bulk-Only Transport / SCSI)
- **Audio :** Sound Blaster 16, Intel HD Audio et ABI PCM/contrôle compatible ALSA
- **Affichage :** Cœur DRM/KMS avec registre générique de pilotes GPU (VirtIO-GPU fourni comme pilote intégré), console framebuffer GOP avec polices bitmap et repli framebuffer logiciel
- **Bus :** PCI/PCIe (ECAM + legacy), contrôleurs hôtes USB (UHCI/OHCI/EHCI/xHCI) et I2C
- **Plateforme :** ACPI, HPET, RTC, port série, port parallèle IEEE 1284 et TPM (TIS/CRB, TPM 1.2/2.0)

## Architecture

Le noyau démarre via Limine, qui passe le contrôle à `kernel_entry()` dans `init/main.c`. L'initialisation précoce démarre l'état SIMD, la sortie série, l'allocateur physique, la pagination et le tas ; puis la couche plateforme détecte ACPI, TPM, TSC et SMP, avant que les pilotes et systèmes de fichiers ne soient enregistrés. La gestion des processus, l'IPC et l'ordonnanceur sont initialisés en dernier, après quoi l'espace utilisateur `init` fourni par le chargeur de démarrage est chargé comme PID 1 et l'ordonnancement commence.

```
Limine (UEFI/Legacy)
               |
               v
+------------------------------+     +-------------------------------+
| Initialisation précoce       |---->| Plateforme et pilotes         |
| FPU/SSE -> série -> alloc    |     | ACPI -> SMP -> PCI -> stockage|
| pagination -> tas -> modules |     | réseau -> audio -> entrée -> USB |
+------------------------------+     +-------------------------------+
               |                                    |
               v                                    v
+------------------------------+     +-------------------------------+
| VFS et systèmes de fichiers  |     | Services noyau                |
| tmpfs/procfs/sysfs -> FAT    |     | ordonnanceur -> processus -> IPC |
| ext/NTFS/ISO9660             |     | appels système -> signaux -> cgroups |
+------------------------------+     +-------------------------------+
               |                                    |
               +-----------------+------------------+
                                 |
                                 v
                   sched_start() -> init (PID 1)
```

## Pour commencer

### Prérequis

- **make**, **gcc** (13.3+ recommandé), **qemu**, **xorriso**
- **clang-format**, **clang-tidy** (formatage et analyse statique)
- **kconfig-frontends** + **libncurses-dev** (pour `menuconfig`)

Debian/Ubuntu :

```bash
sudo apt update
sudo apt install make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

ArchLinux :

```bash
pacman -Sy make gcc qemu-system xorriso clang-format clang-tidy kconfig-frontends libncurses-dev dos2unix
```

### Compilation

```bash
git clone https://github.com/ViudiraTech/Uinxed-Kernel.git
cd Uinxed-Kernel
make
```

Cela produit `UxImage` (image noyau) et `Uinxed-x64.iso` (image CD amorçable).

### Exécution dans QEMU

```bash
make run
```

`make run` démarre l'ISO avec `-machine q35`, le firmware OVMF et `-serial stdio`, donc la sortie série apparaît dans votre terminal.

### Exécution sur matériel physique

**Mode UEFI**

1. Convertissez le disque cible en table de partitions GPT et créez une ESP.
2. Copiez le contenu de `./assets/Limine` dans l'ESP.
3. Copiez `UxImage` dans `EFI/Boot/` sur l'ESP.
4. Démarrez en mode UEFI 64 bits (Secure Boot désactivé).

**Mode Legacy**

1. Gravez `Uinxed-x64.iso` sur un disque.
2. Démarrez dessus sur une machine 64 bits.

Les deux modes fonctionnent également via [Ventoy](https://www.ventoy.net/) : copiez simplement l'ISO sur le disque et sélectionnez-la dans le menu de démarrage.

## Structure du projet

```
Uinxed-Kernel/
|-- assets/           # Ressources de compilation et de démarrage
|-- boot/             # Structures et interfaces du protocole de démarrage
|-- docs/             # Documentation du projet et notes techniques
|-- drivers/          # Pilotes matériels et support des périphériques
|-- fs/               # Implémentations de systèmes de fichiers et composants VFS
|-- include/          # En-têtes noyau et interfaces publiques
|-- init/             # Point d'entrée noyau et routines d'initialisation
|-- ipc/              # Mécanismes de communication inter-processus
|-- kernel/           # Sous-systèmes noyau principaux et services d'exécution
|-- libs/             # Bibliothèques noyau internes et fonctions utilitaires
|-- mem/              # Sous-système de gestion de la mémoire
|-- net/              # Pile réseau et implémentations de protocoles
|-- scripts/          # Scripts de compilation, configuration et maintenance
|-- security/         # Composants de sécurité et de politique
|-- tools/            # Outils de développement, débogage et auxiliaires
|-- .clang-format     # Configuration du formatage de code
|-- .clang-tidy       # Configuration de l'analyse statique
|-- .config-default   # Configuration noyau par défaut
|-- .gitignore        # Règles d'ignorance Git
|-- CONTRIBUTING.md   # Guide de contribution
|-- CREDITS           # Contributeurs
|-- Kconfig           # Système de configuration noyau
|-- LICENSE           # Licence du projet (Apache License 2.0)
|-- Makefile          # Système de compilation
|-- NOTICE            # Licences tierces et mentions d'attribution
|-- README.md         # Aperçu et présentation du projet
`-- SECURITY.md       # Politique de sécurité et signalement des vulnérabilités
```

## FAQ

**Quelles commandes de développement sont disponibles ?**

Exécutez `make help` pour lister toutes les cibles prises en charge (format, check, menuconfig, gen.clangd, etc.).

**Erreurs "XXX.h file not found" dans l'éditeur ?**

Si vous utilisez clangd comme serveur LSP, générez la configuration du projet :

```bash
make gen.clangd
```

Pour les autres serveurs LSP, adaptez votre configuration à partir du Makefile.

**Comment lire les journaux du noyau ?**

La sortie des journaux est envoyée à la console de démarrage. Par défaut, elle va à l'écran (`tty0`, défini via `kernel_cmdline: console=tty0` dans `assets/Limine/Limine/limine.conf`). Pour capturer les journaux via le port série :

1. Changez `kernel_cmdline` dans `limine.conf` en `console=ttyS0` (ou `ttyS1`-`ttyS3`).
2. Recompilez et exécutez. `make run` connecte déjà la sortie série de QEMU à votre terminal (`-serial stdio`).

Le paramètre `console=` accepte `tty0` (écran VGA) et `ttyS0`-`ttyS3` (ports série). Notez que la console écran met en mémoire tampon la sortie et peut perdre des données si la file VGA déborde ou si le noyau se bloque pendant le démarrage ; la console série est un moyen fiable de déboguer les blocages. Les messages de débogage `plogk` ne sont compilés que lorsque `CONFIG_KERNEL_LOG` est activé.

**Tous les appels système Linux sont-ils implémentés ?**

Non. La table des appels système suit la numérotation Linux 6.12 x86-64 (appels système 0-462), mais seul un sous-ensemble est implémenté. Les appels système non implémentés renvoient `-ENOSYS` au lieu de planter, et l'ensemble implémenté grandit au fur et à mesure que le projet évolue.

**Pourquoi un pilote attendu ne fonctionne-t-il pas (par ex. VirtIO-GPU, SB16) ?**

Certains sous-systèmes sont désactivés par défaut dans Kconfig. Par exemple, `VIRTIO` et `VIRTIO_GPU` sont par défaut `n`, et `SOUND_SB16` est par défaut `n`. Activez-les via `make menuconfig` (kconfig-frontends et libncurses-dev requis), puis assurez-vous que le périphérique correspondant existe dans votre VM ou sur votre matériel. Les pilotes GPU sont découverts via le registre dans `drivers/gpu/gpu_drivers.c` ; ajouter un nouveau pilote GPU signifie ajouter une entrée là-bas. La compilation lit `.config` s'il existe, sinon `.config-default` ; le `.config` généré a la priorité.

**Puis-je l'exécuter sur du vrai matériel ?**

Oui, mais traitez-le comme un noyau expérimental. Suivez les étapes pour le matériel physique ci-dessus et privilégiez les machines jetables ou les disques de test — les pilotes de systèmes de fichiers (en particulier l'écriture NTFS) ne sont pas encore sûrs pour des données importantes.

## Licence et avertissement

### Licence

Ce projet est sous licence [Apache License 2.0](../LICENSE).
Cette distribution inclut des logiciels tiers, chacun sous sa propre licence. Les attributions complètes et les textes de licence sont regroupés dans [NOTICE](../NOTICE).
Une partie de la base de code référence et réimplémente des interfaces du noyau Linux pour l'interopérabilité. Ce sont des implémentations indépendantes d'interfaces et de spécifications publiques ; elles ne contiennent pas de code source du noyau Linux.

### Avertissement

Uinxed est un noyau expérimental en développement actif. Il est fourni "tel quel", sans garantie d'aucune sorte, expresse ou implicite, y compris, mais sans s'y limiter, les garanties de qualité marchande et d'adéquation à un usage particulier.

- Le noyau et ses pilotes de systèmes de fichiers (y compris l'écriture NTFS) ne sont **pas** sûrs pour des données de production. Utilisez uniquement des disques jetables ou des machines virtuelles.
- La prise en charge matérielle est incomplète ; l'exécution sur du matériel réel non testé peut entraîner des blocages, des crashs ou une perte de données.
- Ce projet n'est pas affilié à Linux, Limine ou à tout projet open source mentionné. Toutes les marques appartiennent à leurs propriétaires respectifs.

En utilisant ce logiciel, vous reconnaissez le faire à vos propres risques.

## Contacts

- E-mail : rainy101112@163.com | 2609948707@qq.com | 3585302907@qq.com
- Discord : [Rejoindre le serveur](https://discord.gg/nTkg7HCpy7)
- Groupe QQ : [983673299](https://qm.qq.com/q/8goacFf1iU)
