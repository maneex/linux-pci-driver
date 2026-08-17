# VELOCITOR

Accélérateur PCIe de calcul matriciel **fictif** : un modèle de périphérique
QEMU, un driver noyau Linux et le runtime utilisateur qui va avec.

Le contrat est [`velocitor-device-spec.md`](velocitor-device-spec.md) (v0.6.3).
Toute divergence entre une implémentation et ce document se tranche en
corrigeant le document d'abord (§0.6).

Le livrable n'est pas le code, ce sont les mesures — voir §0.3 et §12 de la
spec.

## Organisation

| Répertoire | Contenu | Écrit par |
|---|---|---|
| `qemu-device/` | le modèle QEMU : périphérique, firmware simulé, moteurs, compteurs | intervenant « modèle » (§0.6) |
| `module/` | `velocitor_pci` : driver noyau, remoteproc, rpmsg, virtio, UAPI | intervenant « driver » (§0.6) |
| `devtools/` | environnement de compilation et d'exécution en VM | commun |
| `vm/` | images de VM (hors dépôt) | — |

## Prérequis

Debian 12 / Ubuntu :

```sh
sudo apt install build-essential ninja-build pkg-config python3 python3-venv \
                 libglib2.0-dev libpixman-1-dev libcap-ng-dev libattr1-dev \
                 flex bison bc libelf-dev libssl-dev cpio curl git
```

`libcap-ng-dev` et `libattr1-dev` sont ceux dont QEMU a besoin pour virtfs,
donc pour le partage 9p. Sans eux QEMU se construit quand même, virtfs se
désactive tout seul, et le boot échoue plus tard sur `virtio-9p-pci is not a
valid device model name` — `build-qemu.sh` les contrôle donc à l'entrée.

## Démarrage

```sh
devtools/setup.sh          # noyau invité 6.18.44 + busybox + initramfs (~15 min)
devtools/build-qemu.sh     # QEMU 7.2.22 avec le device velocitor (~10 min)
devtools/qtest-probe.sh    # vérifie le modèle seul, sans invité (2 s)
devtools/build-module.sh   # module/ contre le noyau invité
devtools/boot.sh           # démarre la VM, device attaché
```

Les scripts sont idempotents : relancer ne refait que ce qui a changé.
Éditer `qemu-device/velocitor.c` puis relancer `build-qemu.sh` ne relance que
`ninja`.

Dans l'invité :

```sh
lspci                                        # busybox : Class 1200: 1b36:0100
grep . /sys/bus/pci/devices/*/vendor         # marche toujours
insmod /mnt/velocitor/module/velocitor.ko
dmesg | tail
```

Le shell de l'invité est l'`ash` de busybox et `lspci` son applet, pas celui de
pciutils : ni `-nn`, ni `-vv`, ni expansion d'accolades. Pour la config space
complète, `hexdump -C /sys/bus/pci/devices/0000:00:03.0/config`.

### Sortir

| Quoi | Effet |
|---|---|
| `Ctrl+D`, `exit`, `poweroff -f` | éteint l'invité proprement, QEMU rend la main |
| **`Ctrl-A` puis `X`** | tue QEMU **depuis n'importe quel état**, panic compris |
| `Ctrl-A C` | bascule vers le moniteur QEMU ; `Ctrl-A H` liste le reste |

`Ctrl-A X` marche parce que `-nographic` multiplexe le moniteur QEMU sur le
même terminal que la console série. C'est la seule sortie qui ne dépend pas de
l'invité — à retenir avant d'en avoir besoin.

Deux réglages qui évitent de rester bloqué, et qui n'allaient pas de soi :

- **le shell de l'invité n'est pas PID 1.** `init` le lance en fils et éteint
  la machine quand il sort. S'il était `exec`é, Ctrl+D — un EOF ordinaire —
  ferait sortir PID 1, et le noyau répond à ça par
  `Kernel panic - not syncing: Attempted to kill init!`, terminal bloqué.
- **`panic=1` sur la ligne de commande noyau.** Avec le `-no-reboot` de QEMU,
  le redémarrage que le noyau demande après un panic fait sortir QEMU au lieu
  de redémarrer : un panic rend le terminal au lieu de le prendre en otage, et
  un `--test` échoue vite au lieu d'attendre son délai. Réglable par
  `GUEST_PANIC_TIMEOUT`, `0` pour rester assis sur le panic.

### Le partage 9p

Le dépôt est monté dans l'invité sur `/mnt/velocitor/` : on recompile le
module sur l'hôte, on `insmod` dans l'invité, sans reconstruire ni initramfs
ni image disque.

9P est le protocole de fichiers en réseau du Plan 9 des Bell Labs — une
poignée de messages (`Twalk`, `Topen`, `Tread`, `Twrite`) au-dessus d'un
transport quelconque. Linux en a un client, `CONFIG_9P_FS`. QEMU en a un
serveur qui sert un répertoire de l'hôte. Entre les deux, pas de réseau : le
transport est `virtio` (`CONFIG_NET_9P_VIRTIO`), donc un anneau de
descripteurs en mémoire partagée — le même mécanisme que le §8 de la spec
utilise pour son plan de données.

Concrètement, trois pièces :

| Où | Quoi |
|---|---|
| `boot.sh` | `-virtfs local,path=<dépôt>,mount_tag=velocitor,security_model=none` |
| `initramfs/init` | `mount -t 9p -o trans=virtio,version=9p2000.L velocitor /mnt/velocitor` |
| `kernel.config` | `CONFIG_9P_FS`, `CONFIG_NET_9P`, `CONFIG_NET_9P_VIRTIO` |

Le `mount_tag` est le nom par lequel l'invité désigne le partage ;
`security_model=none` dit au serveur de ne pas essayer de reproduire les
UID/GID de l'invité sur les fichiers de l'hôte — sans quoi il lui faudrait des
privilèges. C'est acceptable ici parce que l'invité est jetable et que le
partage sert à lire du code, pas à faire autorité sur des permissions.

Côté hôte, ce serveur 9p est la fonctionnalité `virtfs` de QEMU, d'où les deux
paquets `-dev` des prérequis.

### Réglages

Créer `devtools/config.local` (ignoré par git) ; il est sourcé après
`config.defaults` et l'emporte sur tout :

```sh
QEMU_MEM="2G"
QEMU_SMP="4"
```

Quelques usages :

```sh
devtools/boot.sh --gdb                       # attend gdb sur :1234
devtools/boot.sh --test 'insmod /mnt/velocitor/module/velocitor.ko; dmesg | tail'

# Configuration vIOMMU du §14, pour produire des IOVA hauts (§9.1)
QEMU_MACHINE=q35,kernel-irqchip=split KERNEL_CMDLINE_EXTRA=intel_iommu=on \
  devtools/boot.sh -- -device intel-iommu,intremap=on,aw-bits=48
```

## Le modèle QEMU

`qemu-device/` contient deux fichiers, installés dans `hw/misc/` d'un arbre
QEMU au tag épinglé par `build-qemu.sh`, qui applique aussi la colle
`meson.build` et `Kconfig`. Le modèle est un **device in-tree** (§C.4) : il se
compile avec QEMU, il ne se charge pas dedans. L'arbre est dans
`devtools/.cache/qemu-7.2.22/` ; on n'y édite rien, la source est
`qemu-device/`.

| Fichier | Rôle |
|---|---|
| `velocitor_hw.h` | constantes partagées : identité PCI, §2 en entier, carte des registres BAR0 du §4 |
| `velocitor.c` | le modèle |

`velocitor_hw.h` est l'amorce de **l'en-tête partagé** réclamé par §2 et §C.2 :
il ne dépend d'aucun en-tête QEMU, noyau ou libc, précisément pour pouvoir
être consommé tel quel par les trois côtés. Le driver l'a adopté :
`module/include/pci.h` a disparu, et `module/Makefile` ajoute
`-I$(src)/../qemu-device/`.

### Ce qui est implémenté aujourd'hui

Les étapes 2 et 3 du §13, des deux côtés :

- identité PCI et disposition des capacités (§2.1, §3) ;
- les trois BAR, avec type et taille contractuels ;
- le bloc d'identité de BAR0 et `SCRATCH` (§4.1) : `MAGIC`, `VERSION`, `CAPS`,
  `SCRATCH`, `MEM_SIZE`, `TOPOLOGY`, `DMA_BITS` ;
- les 20 compteurs du §4.5, avec `CNT_SNAP` et `CNT_RESET` : les lectures
  répondent depuis l'instantané, jamais depuis les valeurs vives ;
- MSI-X sur BAR4, six vecteurs, plus `IRQ_STATUS` / `IRQ_MASK` / `IRQ_ACK`
  (§3.3, §4.1) — seuls les vecteurs 0 et 5 latchent ;
- `ERR_INJECT` bit 2 (§9), le déclencheur qui rend l'étape 3 falsifiable :
  `FW_STATUS = 3` et vecteur 5 ;
- les règles d'accès du §4 : 32 bits alignés uniquement, sinon `0xFFFFFFFF` en
  lecture et écriture ignorée ; offset non implémenté → lecture 0 ; et les
  registres WO du §A.3 rendent `0xFFFFFFFF` quand on les lit.

Tout le reste de BAR0 répond comme réservé et journalise sous `LOG_UNIMP` en
nommant la section de spec qui l'implémentera. BAR2 est déclarée mais vide.
Rien ne démarre de firmware ni ne déplace de données ; l'erreur levée par
l'injection n'est pas encore *qualifiée*, le bloc `ERR_CODE` du §4.4 n'existant
pas.

Pour voir ces journaux :

```sh
devtools/boot.sh -- -d guest_errors,unimp
```

### Vérifier le modèle sans invité

```sh
devtools/qtest-probe.sh      # 2 secondes, pas de noyau, pas de boot
```

C'est la **couche 1 du §13.1** — le device QEMU sans Linux. L'accélérateur
`qtest` de QEMU permet de piloter MMIO et port I/O sans CPU ni invité : le
script programme les BAR à la main par les ports de configuration
`0xcf8`/`0xcfc`, relit les registres et compare à des valeurs attendues.

Ce qu'il couvre aujourd'hui : `MAGIC`, `VERSION`, `CAPS`, `MEM_SIZE`,
`TOPOLOGY`, `DMA_BITS`, la relecture inversée de `SCRATCH`, les quatre règles
d'accès du §4 (1 octet, 8 octets, non aligné, offset réservé), le rejet d'une
écriture sur registre en lecture seule, la BAR2 stub, le bloc de compteurs et
ses deux registres en écriture seule, la capacité MSI-X et ses deux champs BIR
lus en espace de configuration, et le cycle complet de l'étape 3 — injection,
`FW_STATUS = 3`, latch du vecteur 5, `IRQ_ACK`, retour à zéro. 36 cas.

Le dernier de ces cas est le seul test réel de l'instantané du §4.5 :
`CNT_NOTIFY_TX` lit `0` avant `CNT_SNAP` et `1` après, sur le même
compteur vif. Tant que rien ne s'incrémentait, le mécanisme n'était pas
falsifiable.

L'énumération elle-même — `1b36:0100`, classe `0x1200`, BAR0 4 Ko
non-prefetchable, BAR2 32 Mo 64 bits prefetchable, BAR4 8 Ko, aucun pin
INTx — se lit au moniteur :

```sh
printf 'info pci\nquit\n' | \
  devtools/.cache/qemu-7.2.22/build/qemu-system-x86_64 \
    -M q35 -display none -monitor stdio -S -device velocitor
```

### Et avec l'invité

```sh
devtools/boot.sh --test 'lspci; head -5 /sys/bus/pci/devices/0000:00:03.0/resource'
```

Ce que Linux 6.18.44 en voit, et qui correspond au §3 :

```
00:03.0 Class 1200: 1b36:0100
0x00000000febd7000 0x00000000febd7fff 0x0000000000040200   BAR0  4 Ko, non-prefetchable
0x0000000000000000 0x0000000000000000 0x0000000000000000   BAR1  vide
0x00000000fa000000 0x00000000fbffffff 0x000000000014220c   BAR2 32 Mo, 64 bits, prefetchable
0x0000000000000000 0x0000000000000000 0x0000000000000000   BAR3  mangé par BAR2
0x00000000febd4000 0x00000000febd5fff 0x0000000000040200   BAR4  8 Ko
```

Et `cma: Reserved 128 MiB` dans `dmesg`, le préalable du §2.

Le module *bind* désormais, et ses six vecteurs MSI-X apparaissent dans
`/proc/interrupts` de l'invité.

### Ce qui ne l'est pas

Par étape du §13, dans l'ordre des dépendances :

| Étape | Manque côté modèle |
|---|---|
| 4 | contenu de BAR2 : aperture fixe, fenêtre glissante, `WIN_BASE` et son *read-back* (§3.1, §9) |
| 5 | `DBG_DMA_*` et le bus-mastering (§4.3, annexe D.1/D.2) |
| 6 | `RESET`, `FW_STATUS`, `GENERATION`, en-tête firmware, anneau de trace, table fantôme (§4.2, §6) |
| 7 | endpoint rpmsg et annonce *name service* (§7.1, annexe D.5) |
| 8 | consommation des *split rings*, moteurs GEMM (§8) |
| 9 | erreur qualifiée (§4.4) et `ERR_INJECT` (§9) |

## Divergences et décisions à valider

Deux points sur lesquels le code prend position ; les noter au §16 quand ils
sont tranchés. Les décisions déjà tranchées pendant les étapes 2 et 3 y sont
consignées.

1. **`VERSION` n'est pas fixé par la spec.** Le modèle rend `0.6`
   (`major << 16 | minor`), suivant la révision du contrat implémenté, pour que
   le driver puisse refuser un modèle plus ancien que lui. À entériner ou à
   remplacer.
2. **`CAPS` vaut `0x7`** — fp32, bf16, transposition. C'est le matériel qui
   les a ; ce qui se négocie à l'étape 11 est leur *usage* (§8.1). L'écart
   `CAPS` / `INFO` reste le diagnostic croisé du §7.3, il n'est pas préjugé
   ici.

Deux contraintes d'environnement :

- **La machine doit être `q35`.** `velocitor` est déclaré *endpoint* PCI
  Express : il ne déclare que `INTERFACE_PCIE_DEVICE`, ce dont
  `pci_qdev_realize()` déduit `QEMU_PCI_CAP_EXPRESS`, et il ajoute la capacité
  Express en espace de configuration. Attention, **QEMU 7.2 ne fait pas
  respecter cette contrainte** : sur `-M pc` le device s'énumère quand même,
  capacité Express comprise, sur un bus qui n'a aucune sémantique PCIe. C'est
  donc à nous de tenir la règle. C'est aussi la machine dont le §14 a besoin
  pour le vIOMMU.
- **CMA est obligatoire.** `VEL_HOST_POOL_SIZE` fait 64 Mio, très au-delà de
  `MAX_PAGE_ORDER` : `dma_alloc_coherent()` échoue sans lui (§2). Le noyau
  invité est configuré avec `CONFIG_DMA_CMA` et `boot.sh` passe `cma=128M`.

## Provenance de l'environnement

`devtools/` est repris du **Linux Kernel Module Programming Guide** —
<https://github.com/sysprog21/lkmpg>, répertoire `devtools/`. C'est de là que
viennent le montage noyau + busybox + initramfs + 9p, le mécanisme
d'idempotence par empreinte de configuration, le mode `--test` par
`base64` sur la ligne de commande noyau, et les contournements de
compilation (`-std=gnu11`, `-Wno-error`, `CONFIG_TC` de busybox) qui valent
d'être conservés parce qu'ils coûtent chacun une soirée à retrouver.

Le livre lkmpg est sous OSL-3.0 ; son code d'exemple est sous GPL-2. Voir
`LICENSE` et `GPL-2` dans leur dépôt.

| Fichier ici | Origine | Modifications |
|---|---|---|
| `devtools/setup.sh` | `devtools/setup.sh` | retrait du raccourci « tarball prébuilt » (il télécharge un noyau construit avec *leur* fragment de config, pas le nôtre) et des branches macOS |
| `devtools/boot.sh` | `devtools/boot.sh` | machine `q35`, `-device velocitor`, `cma=`, QEMU local au lieu de celui du système, tag 9p `velocitor` |
| `devtools/build-module.sh` | `devtools/build-modules.sh` | cible `module/` au lieu de `examples/` |
| `devtools/config.defaults` | `devtools/config.defaults` | versions épinglées du §C.4, variables de construction QEMU, réglages invité |
| `devtools/kernel.config` | `devtools/kernel.config` | options des exemples lkmpg retirées ; PCIe, CMA, IOMMU, remoteproc/rpmsg, ftrace ajoutés |
| `devtools/initramfs/init` | `devtools/initramfs/init` | renommages, bannière |
| `devtools/build-qemu.sh` | — | nouveau : pas d'équivalent en amont, lkmpg utilise le QEMU du système |
| `devtools/qtest-probe.sh` | — | nouveau : lkmpg n'a pas de device à tester |

Non repris : `check.sh`, `test-modules.sh`, `guest-test.sh`,
`pack-prebuilt.sh` et les scripts `.ci/`. Ils portent la CI du guide et ses
exemples ; les couches de test du §13.1 sont un autre découpage.
