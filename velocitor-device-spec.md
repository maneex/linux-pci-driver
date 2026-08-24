# VELOCITOR — accélérateur PCIe de calcul matriciel

**Spécification d'un périphérique fictif — v0.6.3**

---

## 0. Contexte du projet — à lire avant toute reprise

Cette section n'est pas de la spécification. Elle existe pour qu'un intervenant qui
découvre ce document comprenne *pourquoi* il est écrit ainsi, et ne « corrige » pas des
choix qui sont délibérés.

### 0.1 But réel

Ce périphérique n'existera jamais. C'est un support d'apprentissage et de démonstration.

Le contexte : apprendre à écrire un driver PCIe.

Le projet vise donc à produire cette preuve, en quelques soirées, sans matériel.

> **Note de publication.** Ce §0 est une note de conception interne. Pour une mise en ligne,
> prévoir trois surfaces distinctes : un `README.md` court (architecture, résultats,
> mesures), un `SPEC.md` purement contractuel (§1 à §9 et annexe D), un `DESIGN.md`
> (rationale, décisions rejetées, limites). La motivation personnelle reste privée ou se
> reformule en objectif d'apprentissage.

### 0.2 Compétences visées

Le projet exerce, dans cet ordre de priorité :

| Compétence | Où |
|---|---|
| Fondamentaux PCIe : BAR, MSI-X, DMA bus-master | §4, §8 |
| Modèle de périphérique Linux, `mmap`, UAPI | §10 |
| Runtime utilisateur au-dessus d'un driver | §10, §11 |
| remoteproc : cycle de vie, carveouts, traduction d'adresses | §6 |
| rpmsg et virtio : plan de contrôle, plan de données, négociation | §7, §8 |
| Ordonnancement, dispatch, placement mémoire | §3, §8 |
| Diagnostic à travers une frontière hôte/device | §9, §11, §12 |
| Émulation comme plateforme de développement testée en CI | §13.1, annexe A.6 |

### 0.3 Le livrable n'est pas le code

C'est le point le plus important, et celui qu'un intervenant risque le plus d'oublier.

Un driver qui marche ne prouve pas grand-chose : n'importe qui peut copier un squelette. Ce
qui a de la valeur, ce sont **les comptages et les comparaisons** — combien de descripteurs
pour un GEMM, l'effet d'un placement distant, ce qui se passe quand un moteur décroche,
comment on identifie une notification perdue en confrontant les compteurs des deux côtés.
Voir §12.

Corollaire pratique : à choisir entre « une fonctionnalité de plus » et « une mesure propre
sur ce qui existe », prendre la mesure.

### 0.4 Pourquoi chaque décision structurante a été prise

**Un firmware simulé plutôt qu'un vrai firmware.** La voie réaliste — un second QEMU, une
chaîne de cross-compilation, un firmware compilé — coûte des semaines et déplace
l'apprentissage vers le bare-metal, qui n'est pas la cible. Le firmware joué en C dans le
modèle QEMU permet à *toute la pile Linux* de tourner pour de vrai.
**Limite assumée et à énoncer** : le projet démontre les protocoles et concepts
remoteproc / rpmsg / virtio, **pas** l'usage de la bibliothèque OpenAMP.

**remoteproc est un détournement délibéré, et il faut le dire.** Un accélérateur d'inférence
réel n'est pas structuré comme un *remote processor* AMP. remoteproc a été choisi parce qu'il
impose un cycle de vie de firmware, une table de ressources et une frontière de protocole
explicites — c'est-à-dire parce qu'il fait travailler plusieurs interfaces noyau d'un coup.
C'est un bon argument. « Les accélérateurs sont faits comme ça » n'en est pas un, et le §0.5
l'interdit déjà en principe.

**Une couche runtime utilisateur, et pas seulement un driver.** C'est la moitié de la
compétence visée, et c'est là que l'expérience C++ système existante rejoint le travail
noyau nouveau. Un driver sans consommateur ne démontre qu'une moitié du sujet.

**Deux plans séparés, rpmsg pour le contrôle et virtio pour les données.** C'est un découpage
courant, et le bon pour cet exercice. rpmsg porte des messages courts et des canaux nommés ;
il n'est pas fait pour du volume.

**Un device virtio propre en plus de rpmsg.** Avec rpmsg seul, virtio est présent mais
invisible : on l'utilise sans jamais manipuler une virtqueue.

**Une seule queue par moteur.** Une virtqueue porte déjà l'aller et le retour : une chaîne
de descripteurs mêle buffers en lecture et en écriture, et `virtqueue_get_buf()` récupère
la requête terminée. Deux queues par moteur seraient redondantes — et le transport
remoteproc plafonne de toute façon à deux vrings par vdev (§6.3).

**Les vrings vivent en mémoire hôte, pas dans une BAR.** `remoteproc_virtio` prend le `va`
du carveout, fait un `memset()` dessus et le passe à `vring_new_virtqueue()` : aucun
`memset_io`, aucune distinction RAM / I/O memory — contrairement au chargeur ELF, qui a bien
les deux chemins. Un vring placé dans une BAR PCI serait donc manipulé comme de la mémoire
normale alors qu'il est du `__iomem`. Les vrings restent donc en mémoire hôte cohérente, et
le device les atteint en bus-master. C'est le comportement de virtio-pci, et c'est plus
proche d'un accélérateur réel. Le carveout `heap` device-local reste, lui, pleinement
pertinent : c'est là qu'est la résidence, qui est le sujet.

**Mais la table de ressources, elle, doit rester visible du device.** C'est la correction
centrale de la v0.6.3. Toutes les `config_ops` de `remoteproc_virtio` — statut virtio,
`dfeatures`, `gfeatures`, espace de configuration — passent par `rproc->table_ptr`. En
laissant `find_loaded_rsc_table` à `NULL`, la v0.6.2 gardait cette table dans la seule
`cached_table` du noyau : **le modèle ne pouvait donc pas savoir quelles features avaient été
négociées**, alors que `VIRTIO_RING_F_EVENT_IDX` et `VIRTIO_RING_F_INDIRECT_DESC` changent
l'analyse de l'anneau. La table revient donc, mais en **mémoire hôte cohérente** et non dans
une BAR : l'objection qui l'avait fait disparaître — un `memcpy()` ordinaire vers du
`__iomem` — ne s'applique pas à de la RAM. Voir §6.4.

**Une aperture fixe *et* une fenêtre glissante sur BAR2.** L'aperture basse garantit que le
firmware et le tampon de trace restent atteignables quelle que soit la position de la
fenêtre ; la fenêtre haute permet d'atteindre le reste. Le découpage en deux zones est ce que
font les vrais périphériques. *(La justification d'origine — « la fenêtre seule écraserait
les vrings » — est caduque depuis que les vrings sont en mémoire hôte.)*

**Deux types de tampons, et des copies explicites.** La mémoire hôte projetée par `mmap`
et la mémoire locale du device sont deux espaces distincts ; aucun mécanisme ne les relie
implicitement. D'où `COPY_H2D` et `COPY_D2H` sur le plan de données — et non un retour
d'`UPLOAD`/`DOWNLOAD` en rpmsg. Bénéfice induit : l'ordre de soumission sur une queue donne
une sémantique de flux — copier, calculer, rapatrier s'enchaînent sans aller-retour hôte.

**Un objet `Stream` dans le runtime.** La sémantique de flux ci-dessus n'existe que si les
trois opérations partent sur la même queue. Tant que l'API n'exposait aucun moyen de le
garantir, elle était une convention non exprimable — et le placement sur nœud, qui est le
sujet mesurable du projet, n'était pas décidable depuis l'application. Un stream v1 est une
queue de moteur ; il y en a donc deux.

**Trois espaces de capacités distincts.** `CAPS` décrit le matériel (§4.1), `INFO` décrit ce
que le firmware croit du matériel (§7), les features virtio décrivent ce qui a été négocié
pour le plan de données (§8.1). Ce ne sont pas des doublons à éliminer : l'écart entre les
deux premiers est détectable au *bring-up* et alimente `mismatch` (§11). En revanche, une
capacité du plan de contrôle n'a rien à faire dans les features du plan de données — c'était
le cas de `VEL_F_NODE_HINT`, qui gouvernait `ALLOC` sur rpmsg depuis le vdev de données.

**Deux moteurs via multi-queues, et non via SR-IOV.** SR-IOV répond à une question
d'isolation entre locataires, pas de parallélisme. Il coûte une PF, des VF, un driver VF
distinct, de la configuration IOMMU et une VM pour être crédible — et ne démontre qu'une
capacité à configurer un mécanisme.

**Deux nœuds mémoire avec pénalité d'accès distant.** Transposition, à l'échelle du device,
d'un problème déjà maîtrisé à l'échelle d'un serveur. Rend le placement décidable et
mesurable.

**Une observabilité spécifiée dès le départ.** Doctrine : *on est en bout de chaîne,
l'utilisateur accuse le SDK en premier, il faut pouvoir prouver que le sous-système est
sain — sans recompiler, sans build de debug, chez le client.* Les compteurs device (§4.5)
sont une source de vérité indépendante du driver ; les tracepoints (§11) sont activables à
chaud.

**Une sémantique d'erreur définie dès la v0.1.** L'atomicité et ce qu'on conserve après un
échec ne s'ajoutent pas en version 1.4 : si l'information n'est pas produite, aucun format
de remontée ne la fera exister.

**Une méchanceté délibérée (§9).** Un device docile n'apprend rien. Chaque piège correspond
à un mode de défaillance réel et silencieux — et chacun est **déterministe**, faute de quoi
il ne produirait pas une mesure mais une anecdote.

### 0.5 Ce qu'il ne faut pas faire

- **Ne pas rendre le device gentil.** Les pièges du §9 sont la valeur du projet.
- **Ne pas écrire d'anneau virtio à la main.** Côté driver, `virtqueue_add_sgs()` et
  `virtqueue_kick()` encapsulent le protocole ; on les utilise. Les séquences de barrières
  décrites en annexe A.2 concernent **le modèle QEMU et les structures hors virtio**, pas
  l'usage des virtqueues côté hôte.
- **Ne pas viser la ressemblance avec un produit commercial existant.** Le device est
  fictif, noms et registres inventés.
- **Ne pas ajouter de fonctionnalité avant d'avoir mesuré l'existante.**
- **Ne pas remonter les étapes du §13 dans le désordre.** Les dépendances sont réelles.
- **Ne pas revendiquer plus que ce que le montage démontre.** Voir §0.4 sur OpenAMP et sur
  remoteproc, et l'annexe A.6 sur ce que l'émulation ne prouve pas.

### 0.6 Répartition du travail

Deux implémentations en regard, écrites par deux intervenants différents :

- **le modèle QEMU** — le périphérique, son firmware simulé, les moteurs, les compteurs ;
  ses obligations sont rassemblées en **annexe D** ;
- **le driver noyau et le runtime utilisateur** — `velocitor_pci`, remoteproc, rpmsg,
  virtio, UAPI, `libvelocitor`, debugfs, tracepoints.

Cette séparation est volontaire : elle reproduit la situation d'un ingénieur runtime face à
une équipe firmware. Quand ça ne marche pas, il faut décider lequel des deux ment — c'est
du *bring-up* en miniature.

**La présente spec est le contrat.** Toute divergence se tranche en corrigeant ce document
d'abord, jamais en adaptant silencieusement une des deux implémentations.

### 0.7 État actuel

| Élément | État |
|---|---|
| Spécification | v0.6.3 — close ; cf. C.6 |
| Versions Linux et QEMU épinglées | **Linux 6.18.44 · QEMU 7.2.22** — liste de vérification §C.4 **non exécutée** |
| Modèle QEMU | étapes 2 à 7 faites : identité PCI et les trois BAR, `SCRATCH`, compteurs et `CNT_SNAP`, MSI-X et `IRQ_*`, BAR2 avec aperture fixe, fenêtre glissante et relecture de `WIN_BASE`, bloc `DBG_DMA_*` asynchrone avec le piège des 42 bits, cycle de vie `RESET`/`FW_STATUS`/`FW_ABI`/`GENERATION` avec vérification de l'en-tête firmware, table fantôme et parcours des `notifyid`, fenêtre `VQ_*` complète, `DOORBELL` routé par `notifyid`, balayage de l'anneau `avail`, purge des queues au reset et au crash (D.3, §6.5), §4.4 complet avec `ERR_DROPPED`, le journal du firmware dans l'anneau de trace du §6.6, et l'endpoint rpmsg de D.5 : consommation des *split rings*, anneau `used`, annonce *name service* et réponse à `INFO`. l'allocateur *bump* par nœud du §14 et les trois opérations qui s'appuient dessus — `ALLOC`, `FREE`, `STAT` (§7.2) — avec le bit 5 d'`ERR_INJECT`. Reste tout le §8 |
| Firmware | généré — `firmware/mkfw.c` produit `velocitor-fw.elf` : en-tête du §6.6 et table de ressources du §6.3 (carveout `heap`, deux vdev de deux vrings). Aucun code, le modèle joue le processeur |
| Driver noyau | étapes 2 à 6 faites : `probe` en devres intégral, BAR0 et BAR2 mappées, masque DMA et pool cohérent, six vecteurs MSI-X, lecture fenêtrée de la mémoire device, transferts `DBG_DMA_*` avec attente bornée, `rproc_ops` complet — carveouts, `da_to_va`, `find_loaded_rsc_table`, table fantôme, boot jusqu'à `FW_STATUS = 2` —, module `vring` autonome, parcours de `cached_table` pour les `notifyid`, fenêtre `VQ_*` programmée dans `ops->start()` donc rejouée à la reprise, `rproc_vq_interrupt()` routé par cookie d'IRQ, `rproc_report_crash()` sur le vecteur d'erreur, debugfs `counters` / `counters_reset` / `inject_error` / `dma_pool` / `dma_ctrl` / `mem`, tracepoints `irq`, `irq_cfg`, `error`, `winmove` et `dma_dbg` — le premier diverge du §11, cf. §14. Reste de l'étape 6 : `TOPOLOGY`/`MEM_SIZE`/`CAPS`, vérification de `FW_ABI`, `GENERATION`, et l'anneau de trace en debugfs |
| Runtime utilisateur | non commencé |
| Tests | couche 1 — `devtools/qtest-probe.sh`, 163 vérifications sans Linux, transport compris : parcours des `notifyid`, fenêtre `VQ_*`, routage du `DOORBELL`, balayage de l'`avail`, purge au reset, `ERR_DROPPED`, et le plan de contrôle de bout en bout — `ALLOC` avec son `dev_offset` au bas du nœud 0, `STAT` et l'asymétrie 112/128 Mio du §3.2, `FREE` puis le même `FREE` refusé en `ERR_CODE = 3`, et le bit 5 d'`ERR_INJECT` qui se désarme entre deux allocations. Couche 2 — `devtools/guest-dma-test.sh`, aller-retour DMA, contrôle croisé fenêtre / DMA et passe de *fuzzing* rejouable par graine ; `devtools/guest-trace-test.sh`, anneau de trace, avec le contrôle croisé `dropped` / `skipped` entre les deux implémentations. Couche 3 inexistante |
| En-tête partagé des constantes | écrit — `qemu-device/velocitor_hw.h`, consommé tel quel par le modèle, le driver et le générateur de firmware |

### 0.8 Glossaire minimal

| Terme | Sens ici |
|---|---|
| **BAR** | *Base Address Register* : fenêtre d'adresses physiques routée vers le device |
| **aperture** | région de BAR mappant une plage fixe de la mémoire device |
| **fenêtre glissante** | région de BAR dont l'origine dans la mémoire device est reprogrammable |
| **handle** | référence opaque vers un bloc alloué en mémoire device ; **jamais nul** |
| **nœud** | moitié de la mémoire locale, proche d'un moteur et distante de l'autre |
| **moteur** | unité de calcul GEMM ; il y en a deux, chacun servi par sa queue |
| **stream** | file d'opérations strictement ordonnée ; en v1, un stream est une queue de moteur |
| **carveout** | région mémoire déclarée dans la table de ressources et gérée par remoteproc |
| **`da` / `va`** | *device address* vue par le firmware / adresse virtuelle côté hôte |
| **remoteproc** | sous-système Linux de gestion du cycle de vie d'un processeur distant |
| **rpmsg** | messagerie par canaux nommés au-dessus de virtio, pour le plan de contrôle |
| **vdev** | *virtio device* déclaré dans la table de ressources remoteproc |
| **table fantôme** | copie de la table de ressources en mémoire hôte cohérente, lisible par le device (§6.4) |
| **`notifyid`** | identifiant de notification attribué par remoteproc à chaque vring |
| **génération** | numéro incrémenté à chaque démarrage du firmware ; invalide les handles antérieurs |
| **jeton** | `{generation, seq}` identifiant une opération en vol (§10.2) |
| **HostBuffer / DeviceBuffer** | mémoire hôte projetée / mémoire locale du device — **deux choses distinctes** |

---

## 1. Vue d'ensemble

```
┌───────────────── HÔTE (x86, Linux) ─────────────────┐
│  application                                        │
│      │  libvelocitor  (C++, RAII, Stream, async)    │
│      │       │                                      │
│      └───────┴── /dev/velocitor  (UAPI: ioctl+mmap) │
│                      │                              │
│  driver velocitor_pci                               │
│   ├── remoteproc     : ELF, carveouts, cycle de vie │
│   ├── vdev0 : rpmsg  : plan de contrôle             │
│   ├── vdev1 : virtio : plan de données (2 moteurs)  │
│   └── debugfs + tracepoints                         │
└──────────────────────┬──────────────────────────────┘
                       │ PCIe
┌──────────────────────┴──────────────────────────────┐
│  VELOCITOR                                          │
│   ├── BAR0 : registres de contrôle et compteurs     │
│   ├── BAR2 : aperture fixe + fenêtre glissante      │
│   ├── mémoire locale, deux nœuds                    │
│   ├── 2 moteurs GEMM, un par nœud                   │
│   ├── firmware (joué par le modèle QEMU)            │
│   └── moteur DMA maître du bus                      │
└─────────────────────────────────────────────────────┘
```

**Trois étages côté hôte.** Une bibliothèque C++ pour l'application, une UAPI minimale
exposée par un char device, un driver PCIe. C'est la chaîne complète « runtime + driver »
que le projet doit démontrer.

**Deux plans séparés.** Le contrôle passe par rpmsg — peu de trafic, petits messages,
canaux nommés. Les données passent par une virtqueue par moteur.

**Où vit quoi.** Les vrings, la table fantôme et les tampons de données de l'application
vivent en **mémoire hôte cohérente**, que le device atteint en bus-master. La **mémoire
locale** du device ne porte que le firmware, son en-tête, le tampon de trace et le tas des
`DeviceBuffer`. Ces deux espaces ne sont jamais reliés implicitement : `COPY_H2D` et
`COPY_D2H` sont les seuls chemins.

**Deux moteurs, deux nœuds mémoire.** Chaque moteur accède rapidement à son nœud et
lentement à l'autre. Le placement des handles devient une décision mesurable.

**Le firmware est simulé.** Linux parse l'ELF et sa table de ressources, charge les
segments, puis relâche le reset ; le modèle QEMU vérifie l'en-tête chargé, puis exécute une
implémentation C. Toute la pile Linux tourne réellement.

### 1.1 Les trois frontières

Le projet n'en a pas deux mais trois, et c'est la troisième que les révisions successives
ont eu le plus de mal à stabiliser :

```
application
    ↓
UAPI / runtime
    ↓
driver Linux
    ↓
remoteproc_virtio                    ← interface imposée par le noyau
    ↓
TRANSPORT VELOCITOR                  ← la couche que ce document spécifie
    ↓
faux firmware / modèle QEMU
```

Le **transport Velocitor** est composé de cinq choses, et de rien d'autre :

| Élément | Rôle | Où |
|---|---|---|
| table fantôme | ce que remoteproc écrit et que le device doit lire : `gfeatures`, statut, `notifyid` | §6.4 |
| vrings en mémoire hôte cohérente | préalloués par le driver, adoptés par le core | §6.2 |
| fenêtre `VQ_*` | ce que la table 32 bits ne sait pas porter : adresses bus complètes, vecteur MSI-X, activation | §4.2 |
| `DOORBELL` | notification hôte → device | §4.1 |
| MSI-X | notification device → hôte | §3.3 |

Règle de partage entre les deux derniers mécanismes de configuration : **si l'information
existe déjà dans la table de ressources, elle n'est pas dupliquée dans un registre.** Les
`VQ_*` ne portent donc que ce qui ne tient pas dans un `u32` ou qui n'a pas d'équivalent
remoteproc.

---

## 2. Constantes

Partagées entre les implémentations via un en-tête commun. **Première chose à écrire.**

```c
/* --- géométrie du device --------------------------------------------- */
#define VEL_MEM_SIZE        (256u << 20)  /* mémoire locale totale           */
#define VEL_NODES           2             /* nœuds mémoire                   */
#define VEL_ENGINES         2             /* moteurs GEMM                    */
#define VEL_STREAMS         2             /* = VEL_ENGINES en v1, cf. §8.4   */
#define VEL_APERTURE_SIZE   ( 16u << 20)  /* aperture fixe, bas de BAR2      */
#define VEL_WINDOW_SIZE     ( 16u << 20)  /* fenêtre glissante               */
#define VEL_DMA_BITS        42            /* largeur d'adresse DMA supportée */
#define VEL_FAR_PENALTY     4             /* facteur de coût d'accès distant */

/* --- mémoire hôte ----------------------------------------------------- */
#define VEL_HOST_POOL_SIZE  ( 64u << 20)  /* tampon cohérent ; exige CMA     */

/* --- allocation device ------------------------------------------------ */
#define VEL_ALLOC_ALIGN     256u          /* alignement de toute allocation  */
#define VEL_NODE_ANY        0xFFFFFFFFu
#define VEL_ENGINE_ANY      0xFFFFFFFFu

/* --- transport virtio ------------------------------------------------- */
#define VEL_VRING_NUM       256           /* descripteurs par vring          */
#define VEL_VRING_ALIGN     4096u         /* alignement vring ; cf. §6.2     */
#define VEL_MSIX_VECTORS    6
#define VEL_VQ_CTRL_RX      0             /* index VQ_SELECT — voir §4.2     */
#define VEL_VQ_CTRL_TX      1
#define VEL_VQ_ENGINE0      2
#define VEL_VQ_ENGINE1      3

/* --- rpmsg ------------------------------------------------------------ */
#define VEL_RPMSG_NS_ADDR   53            /* adresse réservée du name service */
#define VEL_RPMSG_CTRL_ADDR 1024          /* adresse du service de contrôle  */
#define VEL_CTRL_NAME       "velocitor-ctrl"

/* --- firmware --------------------------------------------------------- */
#define VEL_FW_MAGIC        0x4F465456u   /* en-tête du firmware chargé      */
#define VEL_FW_ABI          1u
#define VEL_FW_HDR_DA       0u            /* l'en-tête est en tête de mémoire */

/* --- trace ------------------------------------------------------------ */
#define VEL_TRACE_SIZE      ( 64u << 10)  /* tampon console firmware         */
#define VEL_TRACE_ENTRY     128u          /* octets par entrée               */
#define VEL_TRACE_ENTRIES   511u          /* (64K - sizeof(hdr)) / 128       */
```

### 2.1 Identifiants

| Identifiant | Valeur | Statut |
|---|---|---|
| PCI Vendor ID | `0x1B36` | valeur QEMU conventionnelle pour un device expérimental |
| PCI Device ID | `0x0100` | provisoire ; changer en cas de conflit dans la version épinglée |
| PCI Class | `0x120000` | *processing accelerator* |
| PCI Revision | `0x01` | |
| `VIRTIO_ID_VELOCITOR` | `0x4000` | délibérément hors de la plage assignée ; à confirmer contre la spec Virtio de la version épinglée (§C.4) |

`VEL_HOST_POOL_SIZE` dépasse largement ce que `dma_alloc_coherent()` sait servir sans CMA :
au-delà de `MAX_PAGE_ORDER` — 4 Mio sur x86 en configuration par défaut — l'allocation
échoue. Or un GEMM 1024×1024 en fp32 demande 4 Mio **par matrice**. **La VM doit donc être
configurée avec CMA**, et c'est un préalable de l'étape 0, pas un réglage de confort (§13).

---

## 3. Identité PCI et mémoire

| BAR | Type | Taille | Contenu |
|---|---|---|---|
| 0 | MEM32, non-prefetchable | 4 Ko | registres de contrôle et compteurs |
| 2 | MEM64, prefetchable | 32 Mo | aperture fixe + fenêtre glissante |
| 4 | MEM32 | 8 Ko | tables MSI-X |

BAR2 étant un BAR 64 bits, il consomme les emplacements 2 et 3 ; les tables MSI-X sont donc
en BAR4.

### 3.1 Disposition de BAR2

```
BAR2 + 0x0000000 ┌────────────────────────────┐
                 │  APERTURE FIXE (16 Mo)     │  → mémoire locale [0, 16 Mo)
                 │  en-tête firmware,         │
                 │  segments firmware,        │
                 │  tampon de trace           │
BAR2 + 0x1000000 ├────────────────────────────┤
                 │  FENÊTRE GLISSANTE (16 Mo) │  → mémoire locale [WIN_BASE, +16 Mo)
BAR2 + 0x2000000 └────────────────────────────┘
```

**Tout le firmware tient sous 16 Mo**, en-tête, segments `PT_LOAD` et tampon de trace
compris. C'est une contrainte contractuelle sur l'ELF, pas une observation : elle garantit
que `load` n'a jamais à déplacer la fenêtre et permet d'utiliser le chargeur ELF générique
(§6.1). **Ni les vrings, ni la table de ressources, ni la table fantôme n'y vivent** — ils
sont en mémoire hôte (§6.2, §6.4).

La fenêtre haute est mobile, origine donnée par `WIN_BASE`, alignée sur `VEL_WINDOW_SIZE`.
Elle est le seul moyen d'atteindre le `heap` depuis l'hôte.

> `WIN_BASE` est une ressource partagée, sans sérialisation matérielle. Deux contextes qui
> la déplacent en concurrence lisent chacun la mémoire de l'autre, sans erreur. Tout accès
> fenêtré doit donc être sérialisé par un verrou du driver.

> **Le chemin fenêtré n'est pas décoratif, mais il n'est pas non plus sur le chemin de
> production.** Il est exercé par l'étape 4 (balayage complet de la mémoire) et par
> `VEL_IOC_PEEK` (§10.2), qui existe pour le diagnostic et pour donner un consommateur réel
> à ce chemin. Le chargeur, lui, ne l'utilise pas.

### 3.2 Nœuds mémoire

| Nœud | Plage | Moteur proche | Allouable |
|---|---|---|---|
| 0 | `[0, VEL_MEM_SIZE/2)` | moteur 0 | `[16 Mo, 128 Mo)` — 112 Mo |
| 1 | `[VEL_MEM_SIZE/2, VEL_MEM_SIZE)` | moteur 1 | `[128 Mo, 256 Mo)` — 128 Mo |

Un accès distant coûte `VEL_FAR_PENALTY` fois plus cher en temps simulé. Le device ne
l'interdit pas et ne prévient pas : il compte, et le driver doit lire les compteurs pour
s'en apercevoir.

> **Les deux nœuds n'ont pas la même capacité allouable** : l'aperture fixe est prélevée sur
> le nœud 0. On ne corrige pas cette asymétrie, on l'expose — `STAT` (§7) rend la capacité
> *et* le libre par nœud, et tout protocole de mesure du §12 doit en tenir compte. Masquer
> l'écart par des carveouts symétriques coûterait de la mémoire pour rendre un chiffre plus
> joli, ce que le §0.3 interdit.

### 3.3 MSI-X, 6 vecteurs

| Vecteur | Source | Acquittement |
|---|---|---|
| 0 | configuration / `FW_STATUS` | `IRQ_ACK` bit 0 |
| 1 | vdev0 vring 0 — **réception côté hôte** | aucun |
| 2 | vdev0 vring 1 — **émission côté hôte** | aucun |
| 3 | vdev1 `engineq0` | aucun |
| 4 | vdev1 `engineq1` | aucun |
| 5 | erreur / crash firmware | `IRQ_ACK` bit 5 |

Les sens de vring 0 et vring 1 sont donnés **du point de vue de l'hôte**, une fois pour
toutes : vring 0 porte ce que le device envoie à Linux, vring 1 ce que Linux envoie au
device. C'est la convention `virtio_rpmsg_bus`, et l'ambiguïté « rx/tx » est le genre
d'inversion qui coûte une soirée à deux implémenteurs qui ne se parlent pas.

Un vecteur par queue, pour router les complétions des deux moteurs vers des cœurs hôtes
différents.

> **Les vecteurs de queue ne s'acquittent pas.** Seules la configuration et l'erreur
> latchent un bit dans `IRQ_STATUS` et exigent une écriture d'`IRQ_ACK`. Le handler d'une
> queue ne lit ni n'écrit aucun registre : c'est le driver qui a programmé la correspondance
> vecteur → `notifyid` (§4.2), donc il la connaît déjà. **Aucun accès MMIO dans le chemin
> d'interruption de données**, et par conséquent aucun champ MMIO dans le tracepoint
> `velocitor_irq` (§11).

---

## 4. Registres — BAR0

Accès 32 bits uniquement. Tout accès non aligné ou de taille différente est ignoré en
écriture et retourne `0xFFFFFFFF` en lecture. Tout offset non décrit ci-dessous est
**réservé** : lecture `0`, écriture ignorée.

Carte générale :

| Plage | Contenu |
|---|---|
| `0x000`–`0x04F` | contrôle (§4.1) |
| `0x050`–`0x06F` | erreur qualifiée (§4.4) |
| `0x070`–`0x08F` | DMA de *bring-up* (§4.3) |
| `0x090`–`0x0EF` | compteurs (§4.5) |
| `0x0F0`–`0x0FF` | table fantôme (§4.2) |
| `0x100`–`0x12F` | fenêtre de configuration de queue (§4.2) |

### 4.1 Contrôle

| Offset | Accès | Nom | Description |
|---|---|---|---|
| `0x000` | RO | `MAGIC` | `0x4F4C4556` (« VELO ») |
| `0x004` | RO | `VERSION` | `major << 16 \| minor` |
| `0x008` | RO | `CAPS` | bit0 fp32, bit1 bf16, bit2 transposition |
| `0x00C` | RW | `SCRATCH` | relecture inversée bit à bit — sonde de mapping |
| `0x010` | RO | `MEM_SIZE` | taille de la mémoire locale |
| `0x014` | RO | `TOPOLOGY` | `VEL_NODES << 16 \| VEL_ENGINES` |
| `0x018` | RO | `DMA_BITS` | largeur d'adresse DMA supportée (42) |
| `0x01C` | RW | `RESET` | 1 = reset assertée, 0 = relâchée |
| `0x020` | RO | `FW_STATUS` | 0 reset, 1 chargé et vérifié, 2 en cours, 3 planté |
| `0x024` | RW | `WIN_BASE` | origine de la fenêtre glissante, cf. §9 |
| `0x028` | RO | `IRQ_STATUS` | bits 0 et 5 uniquement ; cf. §3.3 |
| `0x02C` | RW | `IRQ_MASK` | 1 = masqué |
| `0x030` | WO | `IRQ_ACK` | écrire les bits à effacer |
| `0x034` | WO | `DOORBELL` | valeur = `notifyid` de la vring notifiée |
| `0x038` | RO | `FW_ABI` | version d'ABI lue dans l'en-tête firmware ; `0` tant que non vérifié |
| `0x03C` | RO | `GENERATION` | incrémenté à chaque passage en `FW_STATUS = 2`, cf. §6.5 |
| `0x040` | RW | `ERR_INJECT` | voir §9 |
| `0x044` | RW | `ERR_INJECT_ARG` | paramètre de l'injection active : période ou délai, cf. §9 |

`RSC_OFFSET` n'existe pas : la table de ressources est trouvée dans l'ELF côté Linux, avant
le démarrage (§6). Le device ne la publie pas — il la *reçoit* (§4.2, §6.4).

`DMA_BITS` est en lecture seule et purement informatif. Le driver n'est pas censé s'en
servir : il doit connaître la capacité de son matériel. `DMA_BITS` sert au diagnostic quand
le piège du §9 se déclenche.

`FW_STATUS = 1` est posé par le modèle **au relâchement de `RESET`**, après vérification de
l'en-tête firmware chargé (§6.6). Si le magic ou l'ABI ne concordent pas, le modèle reste en
`FW_STATUS = 0` et lève le vecteur 5 avec `ERR_CODE = 10`. C'est ce qui rend le chargement
falsifiable : un `load` cassé ne peut pas passer inaperçu.

### 4.2 Transport — table fantôme et configuration des queues

**Table fantôme.** Le driver alloue une copie de la table de ressources en mémoire hôte
cohérente (§6.4) et en publie l'adresse bus avant de relâcher `RESET` :

| Offset | Accès | Nom | Description |
|---|---|---|---|
| `0x0F0` | RW | `RSC_ADDR_LO` | adresse bus de la table fantôme, poids faible |
| `0x0F4` | RW | `RSC_ADDR_HI` | poids fort |
| `0x0F8` | RW | `RSC_LEN` | taille en octets |
| `0x0FC` | RW | `RSC_VALID` | 1 = table lisible ; écrit en dernier |

Le device y lit `gfeatures`, le statut virtio et les `notifyid` de chaque vring. Il ne les
écrit jamais : la table appartient à Linux.

**Fenêtre de sélection de queue**, dans l'esprit de virtio-pci. Le driver écrit `VQ_SELECT`,
puis lit et écrit les registres suivants, qui portent alors sur la queue sélectionnée.

| Offset | Accès | Nom | Description |
|---|---|---|---|
| `0x100` | RW | `VQ_SELECT` | index de queue global : `VEL_VQ_*`, 0..3 |
| `0x104` | RO | `VQ_NUM_MAX` | nombre max de descripteurs supporté (`VEL_VRING_NUM`) |
| `0x108` | RW | `VQ_NUM` | nombre de descripteurs effectif |
| `0x10C` | RW | `VQ_ENABLE` | 1 = queue active |
| `0x110` | RW | `VQ_DESC_LO` | adresse bus de la table de descripteurs, poids faible |
| `0x114` | RW | `VQ_DESC_HI` | poids fort |
| `0x118` | RW | `VQ_AVAIL_LO` | adresse bus de l'anneau *available* |
| `0x11C` | RW | `VQ_AVAIL_HI` | |
| `0x120` | RW | `VQ_USED_LO` | adresse bus de l'anneau *used* |
| `0x124` | RW | `VQ_USED_HI` | |
| `0x128` | RW | `VQ_MSIX_VECTOR` | vecteur MSI-X à lever pour cette queue |

L'index de `VQ_SELECT` est **global et contractuel** (`VEL_VQ_CTRL_RX = 0`,
`VEL_VQ_CTRL_TX = 1`, `VEL_VQ_ENGINE0 = 2`, `VEL_VQ_ENGINE1 = 3`), pas relatif au vdev.

Il n'y a pas de registre `VQ_NOTIFYID` : le `notifyid` de chaque vring est déjà dans la
table fantôme, écrit par le core, et le dupliquer créerait deux sources de vérité.

Les vrings étant en mémoire hôte cohérente, ces adresses sont des **adresses bus**, et le
device y accède en maître du bus — par l'espace d'adressage DMA du périphérique PCI, jamais
par la mémoire invitée directement (annexe D).

> **Piège à ne pas rater — la troncature à 32 bits.** `fw_rsc_vdev_vring.da` est un `u32`.
> Le core conserve l'adresse complète dans `rproc_mem_entry::dma` mais assigne
> `mem->da = (u32)dma`, en avertissant si des bits hauts sont perdus.
>
> **Programmer `VQ_DESC/AVAIL/USED` depuis le `dma_addr_t` complet, jamais depuis le `da` de
> la table de ressources.** Sinon, dès qu'un vring atterrit au-delà de 4 Gio, le device lit
> à une adresse tronquée — silencieusement.
>
> C'est particulièrement critique ici, puisqu'on cherche délibérément à produire des IOVA
> hauts pour déclencher le piège des 42 bits (§9). Les deux mécanismes se rencontrent.
>
> Le driver ayant **lui-même alloué les vrings** (§6.2), il détient ces `dma_addr_t` sans
> avoir à fouiller les structures internes de remoteproc — ce qui est aussi bien, puisque
> `rproc_find_carveout_by_name()` n'est pas un symbole exporté.

Ordre d'initialisation par queue : `VQ_SELECT`, adresses, `VQ_NUM`, `VQ_MSIX_VECTOR`, puis
`VQ_ENABLE = 1` en dernier. Le device ignore toute activité sur une queue non activée, et
refuse toute écriture de description sur une queue qui l'est : `VQ_ENABLE` gèle l'anneau
autant qu'il l'ouvre.

**Le driver remet les quatre anneaux à zéro avant de les programmer**, à chaque démarrage
(§5, étape 12). C'est la condition pour que le balayage ci-dessous ne relise pas l'`avail`
d'une génération morte.

> **Au passage de `VQ_ENABLE` à 1, le device doit balayer l'anneau `avail`** comme si un
> doorbell venait d'arriver.
>
> Le motif d'origine a disparu, et il vaut d'être noté. Tant que la fenêtre était programmée
> au retour de `rproc_boot()`, le `virtqueue_kick()` que `virtio_rpmsg_bus` émet dès son
> *probe* arrivait sur une queue encore inactive : ce premier doorbell était perdu par
> construction, et sans balayage à l'activation le plan de contrôle ne démarrait jamais.
> Depuis que la programmation a lieu dans `ops->start()` (§5), la queue est active avant que
> le sous-périphérique n'existe, et le doorbell arrive normalement.
>
> **L'obligation reste**, pour deux raisons : elle coûte une lecture DMA d'un anneau vide, et
> elle tient pour n'importe quel driver — y compris un qui reviendrait à l'ordre précédent,
> ou un second driver écrit contre cette spec. Un contrat qui ne serait vrai que pour une
> implémentation n'est pas un contrat.

### 4.3 DMA de *bring-up*

Bloc de test, hors virtio, destiné à l'étape 5 du §13 : il isole le *bus-mastering* de toute
la plomberie virtio et reste ensuite disponible comme interface de diagnostic.

| Offset | Accès | Nom | Description |
|---|---|---|---|
| `0x070` | RW | `DBG_DMA_ADDR_LO` | adresse bus hôte, poids faible |
| `0x074` | RW | `DBG_DMA_ADDR_HI` | poids fort |
| `0x078` | RW | `DBG_DMA_DEV` | offset dans la mémoire locale |
| `0x07C` | RW | `DBG_DMA_LEN` | octets à transférer |
| `0x080` | WO | `DBG_DMA_CTL` | 1 = H2D, 2 = D2H ; déclenche le transfert |
| `0x084` | RO | `DBG_DMA_STATUS` | 0 inactif, 1 en cours, 2 terminé, 3 erreur |

Le transfert est asynchrone (annexe D) : le driver scrute `DBG_DMA_STATUS` à l'étape 5, où
les interruptions ne sont pas encore en place. Les erreurs de bornes et de largeur d'adresse
alimentent `ERR_CODE` comme n'importe quel autre accès (§4.4), et le transfert incrémente
`CNT_DMA_RD`/`CNT_DMA_WR`.

Ce bloc est aussi le moyen le plus court de provoquer le piège des 42 bits sans monter toute
la pile virtio.

### 4.4 Erreur qualifiée

| Offset | Accès | Nom | Description |
|---|---|---|---|
| `0x050` | RO | `ERR_CODE` | cause de la dernière erreur |
| `0x054` | RO | `ERR_INFO_LO` | contexte : offset ou adresse fautive, poids faible |
| `0x058` | RO | `ERR_INFO_HI` | poids fort |
| `0x05C` | RO | `ERR_NOTIFYID` | vring concernée, `0xFFFFFFFF` si sans objet |
| `0x060` | RO | `ERR_HANDLE` | handle concerné, `0` si sans objet |
| `0x064` | RO | `ERR_GENERATION` | génération au moment de l'erreur |
| `0x068` | RO | `ERR_DROPPED` | erreurs survenues avant acquittement de la précédente |

| `ERR_CODE` | Signification | Nature |
|---:|---|---|
| 0 | aucune erreur | — |
| 1 | descripteur mal formé | fatale |
| 2 | accès hors des bornes d'un handle | synchrone |
| 3 | handle inconnu ou libéré | synchrone |
| 4 | adresse DMA au-delà de `DMA_BITS` | fatale |
| 5 | dimensions GEMM incohérentes | synchrone |
| 6 | type de données non supporté ou non négocié | synchrone |
| 7 | mémoire locale épuisée | synchrone |
| 8 | fenêtre déplacée pendant un accès en cours | fatale |
| 9 | handle d'une génération périmée (`-ESTALE`) | synchrone |
| 10 | en-tête firmware invalide au relâchement de `RESET` | fatale |

Les erreurs **synchrones** remontent dans `vel_resp.status` de la commande fautive et
n'interrompent pas le device ; elles latchent quand même le bloc ci-dessus pour le
diagnostic. Les erreurs **fatales** font passer `FW_STATUS` à 3, lèvent le vecteur 5, et le
driver appelle `rproc_report_crash()` — appelable depuis un contexte d'interruption.

`ERR_DROPPED` est le registre le plus important du bloc : il rend visible ce que le driver
n'a pas vu.

**Acquittement et écrasement.** Une erreur est « vue » quand le driver a acquitté le vecteur
5 : `IRQ_ACK` est le seul signal par lequel l'hôte dit avoir lu le bloc. Tant que ce latch est
levé, une nouvelle erreur **n'écrase rien** et se contente d'incrémenter `ERR_DROPPED`. Le
bloc conserve donc la **première** — la cause — et non la dernière, qui n'est le plus souvent
qu'un symptôme de la cascade qu'elle a déclenchée. Un driver arrivé en retard lit la cause,
plus le compte de ce qui a suivi.

### 4.5 Compteurs

Lecture seule, 32 bits. Source de vérité indépendante du driver.

| Offset | Nom | Description |
|---|---|---|
| `0x090` | `CNT_RESET` | (WO) écrire 1 remet tous les compteurs à zéro |
| `0x094` | `CNT_SNAP` | (WO) écrire 1 fige atomiquement tous les compteurs ci-dessous |
| `0x098` | `CNT_DB_RX` | doorbells reçus |
| `0x09C` | `CNT_NOTIFY_TX` | notifications **décidées** vers l'hôte |
| `0x0A0` | `CNT_NOTIFY_COALESCED` | parmi elles, celles fusionnées avec un réveil déjà en attente |
| `0x0A4` | `CNT_NOTIFY_DROPPED` | parmi elles, celles supprimées par injection (§9 bit 4) |
| `0x0A8` | `CNT_DESC` | descripteurs de transport consommés |
| `0x0AC` | `CNT_GEMM` | GEMM exécutés |
| `0x0B0` | `CNT_DMA_RD` | opérations DMA de *payload* en lecture (device ← hôte) |
| `0x0B4` | `CNT_DMA_WR` | opérations DMA de *payload* en écriture (device → hôte) |
| `0x0B8` | `CNT_BYTES_RD_LO` | octets de *payload* lus depuis l'hôte, poids faible |
| `0x0BC` | `CNT_BYTES_RD_HI` | poids fort |
| `0x0C0` | `CNT_BYTES_WR_LO` | octets de *payload* écrits vers l'hôte, poids faible |
| `0x0C4` | `CNT_BYTES_WR_HI` | poids fort |
| `0x0C8` | `CNT_WIN_MOVE` | déplacements de `WIN_BASE` |
| `0x0CC` | `CNT_FAR_ACCESS` | accès d'un moteur au nœud distant |
| `0x0D0` | `CNT_ERR_DESC` | descripteurs rejetés |
| `0x0D4` | `CNT_ERR_RANGE` | accès hors bornes |
| `0x0D8` | `CNT_STALL_E0` | cycles simulés où le moteur 0 attend du travail |
| `0x0DC` | `CNT_STALL_E1` | idem moteur 1 |
| `0x0E0` | `CNT_CYCLES_E0` | cycles simulés de calcul du moteur 0 |
| `0x0E4` | `CNT_CYCLES_E1` | idem moteur 1 |

**Ce que comptent exactement les compteurs DMA.** `CNT_DMA_RD`/`CNT_DMA_WR` comptent des
**opérations de *payload* logiques** — une `COPY_H2D`, la lecture d'une matrice non
résidente, un transfert `DBG_DMA` — indépendamment du nombre d'accès bus que le modèle émet
pour les réaliser. Le *fetch* des descripteurs, la lecture de l'en-tête de commande et
l'écriture de la réponse relèvent de `CNT_DESC`, jamais des compteurs DMA. Sans cette
règle, l'item 2 du §12 — comparer le nombre de transferts au minimum théorique — n'a pas de
dénominateur.

**Instantané plutôt que verrou de lecture.** Écrire `CNT_SNAP` fige l'ensemble des
compteurs ; les lectures suivantes rendent des valeurs mutuellement cohérentes jusqu'au
prochain `CNT_SNAP`. Aucune lecture n'a d'effet de bord, ce qui préserve la règle de
l'annexe A.3 et supprime la course entre deux lecteurs concurrents — l'ioctl `VEL_IOC_STATS`
et le debugfs `counters` peuvent coexister.

Les compteurs 32 bits **débordent silencieusement** : raisonner en différences.

**Relation attendue entre les compteurs de notification**, sur laquelle repose l'item 7
du §12 :

```
interruptions reçues par le driver
    = CNT_NOTIFY_TX − CNT_NOTIFY_COALESCED − CNT_NOTIFY_DROPPED
```

`CNT_NOTIFY_TX` s'incrémente au moment où le device **décide** de notifier, y compris quand
l'injection supprime ensuite l'interruption. Sans cela, les deux côtés concordent et la
notification perdue reste invisible — ce qui viderait de son sens la démonstration centrale
du projet.

---

## 5. Séquence de démarrage

1. `probe`, `pci_enable_device`, `pci_set_master`, `pci_request_regions`
2. mapper BAR0, vérifier `MAGIC`, sonder `SCRATCH`
3. `dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(VEL_DMA_BITS))` — cf. §9
4. `pci_alloc_irq_vectors` en MSI-X, `VEL_MSIX_VECTORS` vecteurs
5. mapper BAR2, lire `TOPOLOGY`, `MEM_SIZE` et `CAPS`
6. allouer le pool hôte cohérent (`VEL_HOST_POOL_SIZE`) et les **quatre vrings**
7. `rproc_alloc`, puis **`rproc->auto_boot = false`**
8. enregistrer les carveouts : `heap`, puis les quatre vrings sous `vdev%dvring%d` (§6.2)
9. `devm_rproc_add` — sans l'étape 7, cet appel déclencherait un chargement asynchrone du
   firmware qui entrerait en course avec le `rproc_boot()` explicite ci-dessous
10. `rproc_boot` : Linux lit l'ELF, en extrait la table de ressources dans sa `cached_table`,
    résout les carveouts, appelle `load`, puis `find_loaded_rsc_table` — qui rend la table
    fantôme, dans laquelle le core recopie `cached_table` — puis `start`
11. `start` publie `RSC_ADDR_*` et `RSC_VALID`, relâche `RESET`, et attend `FW_STATUS == 2`
12. **toujours dans `start`** : remise à zéro des quatre anneaux, lecture des `notifyid` dans
    la table fantôme, puis programmation de la fenêtre `VQ_*` (§4.2) — adresses bus
    complètes, taille, vecteur MSI-X, et `VQ_ENABLE = 1` en dernier
13. au retour de `ops->start()`, le core démarre les sous-périphériques : les `virtio_device`
    sont enregistrés, leurs drivers appellent `find_vqs()`, les vrings sont construits sur la
    mémoire préallouée, `gfeatures` et le statut sont écrits dans la table fantôme
14. vdev0 → rpmsg, service `velocitor-ctrl` ; vdev1 → driver virtio de données
15. `VEL_IOC_INFO` passe de `BOOTING` à `READY` (§10.2)

> **Pourquoi la fenêtre `VQ_*` est programmée dans `start` et non au retour de
> `rproc_boot()`.** La récupération après crash du §6.5 ne repasse pas par `rproc_boot()` :
> `rproc_boot_recovery()` enchaîne `rproc_stop()` puis `rproc_start()`, donc ni `prepare` ni
> le code appelant ne sont rejoués. Toute programmation placée après le retour de
> `rproc_boot()` n'a lieu qu'une fois dans la vie du device, alors que le §6.5 remet
> justement tous les `VQ_ENABLE` à zéro au plantage. `ops->start()` est le seul point du
> chemin qui soit rejoué à l'identique aux deux démarrages.
>
> **La remise à zéro des anneaux fait partie du contrat, pas de l'hygiène.**
> `vring_new_virtqueue()` remet ses propres compteurs à zéro — `last_used_idx`,
> `avail_idx_shadow` — mais ne touche pas la mémoire de l'anneau, que le driver a allouée.
> À la deuxième génération, `avail->idx` porte donc encore la valeur de la précédente
> pendant que le device, sorti de reset, repart de zéro : il balaierait des descripteurs
> morts. C'est l'ABA du §6.5, transposé du handle vers l'anneau.

> **`FW_STATUS == 2` signifie « firmware démarré », pas « IPC prêt ».** Les virtqueues
> n'existent pas encore côté Linux à ce moment : le core ne démarre les sous-périphériques
> qu'après le retour de `ops->start()`.
>
> **`VQ_ENABLE` reste le contrat de handoff — mais il n'est plus le plus tardif des trois
> signaux.** Il précède désormais `DRIVER_OK`, que `virtio_device_ready()` pose pendant le
> démarrage des sous-périphériques. Le modèle peut donc balayer un anneau que virtio n'a pas
> encore construit ; ce qui rend l'opération sûre est la remise à zéro de l'étape 12, qui
> laisse `avail->idx = 0` — un anneau vide, et exactement l'état sur lequel
> `vring_new_virtqueue()` s'attend à construire.
>
> `VQ_ENABLE` garde son rôle de barrière : **le modèle ne touche à une queue qu'une fois
> celle-ci activée.** Ce qu'il ne garantit plus, c'est que l'hôte ait fini de négocier — d'où
> la relecture de `gfeatures` à chaque usage plutôt qu'une seule fois à l'activation
> (annexe D.4).

Le char device `/dev/velocitor` est enregistré dès l'étape 1, pas à la fin. Il refuse les
opérations avec `-ENODEV` tant que les sous-drivers ne sont pas prêts (§10.1) : c'est ce qui
permet à une application de conserver son descripteur à travers un crash et une reprise, et
donc d'observer ce que le §12 item 6 demande d'observer.

---

## 6. remoteproc — cycle de vie et mémoire

**La vérité est dans l'ELF, côté Linux.** Le driver fournit un `rproc_ops` complet ; le
core remoteproc parse le firmware, localise la table de ressources et résout les ressources
**avant** de démarrer le device. Aucun registre ne publie la table ; le driver en publie
une copie (§6.4).

### 6.1 Callbacks

| Callback | Rôle ici |
|---|---|
| `sanity_check` | `rproc_elf_sanity_check` |
| `get_boot_addr` | `rproc_elf_get_boot_addr` |
| `parse_fw` | `rproc_elf_load_rsc_table` |
| `load` | **`rproc_elf_load_segments`** — le chargeur générique convient |
| `find_loaded_rsc_table` | **custom** — retourne la table fantôme, cf. §6.4 |
| `start` | publie `RSC_ADDR_*`, relâche `RESET`, attend `FW_STATUS == 2` |
| `stop` | active `RESET` |
| `kick` | écrit `DOORBELL` avec le `notifyid` fourni |
| `da_to_va` | traduit une adresse device en pointeur hôte, cf. §6.2 |
| `panic` / `coredump` | optionnels, utiles avec le bit 2 d'`ERR_INJECT` |

> **Pourquoi le chargeur générique suffit désormais.** Tout le firmware tient sous 16 Mo
> (§3.1), donc `da_to_va` peut rendre un pointeur pour n'importe quel segment. Il le rend en
> signalant `is_iomem = true`, et `rproc_elf_load_segments` sélectionne alors `memcpy_toio()`
> et `memset_io()` au lieu de leurs équivalents mémoire. Aucun chargeur maison, aucun accès
> fenêtré dans le chemin de chargement.
>
> **Pourquoi `find_loaded_rsc_table` doit être custom et non le générique.**
> `rproc_elf_find_loaded_rsc_table()` existe et est exportée, mais elle résout la section
> `.resource_table` de l'ELF via `da_to_va` — elle rendrait donc un pointeur BAR2, et
> `rproc_start()` y ferait un `memcpy()` ordinaire vers du `__iomem`. Le callback custom
> ignore l'adresse ELF et rend le tampon cohérent alloué par le driver.

### 6.2 Mémoire device-local — le point délicat

Deux espaces, et le partage n'est pas celui qu'on croit au premier abord.

| Structure | Où | Pourquoi |
|---|---|---|
| vrings des deux vdev | **mémoire hôte cohérente**, allouée par le driver | `remoteproc_virtio` traite le `va` du carveout comme de la mémoire CPU normale : `memset()` puis `vring_new_virtqueue()`, sans jamais passer par `memset_io`. Un vring dans une BAR serait du `__iomem` manipulé comme de la RAM |
| table de ressources | **mémoire hôte**, `cached_table` du core | contrat d'entrée, jamais partagé tel quel |
| table fantôme | **mémoire hôte cohérente**, allouée par le driver | copie de travail du core *et* seule vue du device sur la négociation virtio (§6.4) |
| tampons de données de l'application | **mémoire hôte cohérente**, pool `VEL_HOST_POOL_SIZE` | atteints par le device en bus-master, cf. §8 |
| en-tête et segments du firmware | **mémoire locale**, `[0, 16 Mo)` | écrits par `load` via l'aperture fixe |
| tampon de trace | **mémoire locale**, `[0, 16 Mo)` | lu par le driver via l'aperture fixe, cf. §6.6 |
| tas des `DeviceBuffer` | **mémoire locale**, carveout `heap` | c'est la résidence — le sujet du projet |

**Le driver préalloue les quatre vrings et les enregistre lui-même**, avec
`rproc_mem_entry_init` puis `rproc_add_carveout`, sous les noms `vdev0vring0`, `vdev0vring1`,
`vdev1vring0`, `vdev1vring1`. `rproc_alloc_vring()` cherche un carveout portant exactement ce
nom avant d'en créer un, et adopte celui qu'il trouve.

Trois contraintes contractuelles, faute de quoi l'adoption échoue :

- **taille** exactement `PAGE_ALIGN(vring_size(VEL_VRING_NUM, VEL_VRING_ALIGN))` ;
- **`da = FW_RSC_ADDR_ANY`** dans la table de ressources *et* dans le carveout enregistré —
  c'est la seule combinaison où le contrôle de concordance du core est vide ;
- **propriété** : le driver alloue, donc le driver libère, après `rproc_del` et jamais avant.

Bénéfice : le driver détient les `dma_addr_t` complets sans parcourir `rproc->carveouts` ni
`rproc->rvdevs`, et sans dépendre de `rproc_find_carveout_by_name()`, qui n'est pas exportée.

Le carveout `heap` est lui aussi pré-enregistré, avec des callbacks `alloc`/`release`
propres, mais il n'est pas mappé.

`da_to_va` couvre **l'aperture fixe uniquement** : il retourne un pointeur `__iomem` dans
BAR2 avec `is_iomem = true` pour `da < VEL_APERTURE_SIZE`, et **`NULL` au-delà**.

> **Attention au repli du core.** Si `da_to_va` rend `NULL`, `rproc_da_to_va()` parcourt
> ensuite `rproc->carveouts` et calcule `carveout->va + offset`. Le carveout `heap` étant
> enregistré avec `va = NULL`, ce calcul produirait un pointeur non nul et parfaitement
> invalide dès que `offset > 0`. Le `heap` doit donc être enregistré avec un `va` **et** une
> longueur tels qu'aucun `da` du heap ne tombe dans une entrée mappée — en pratique, ne
> jamais renseigner `va` pour le heap et vérifier au *bring-up* que
> `rproc_da_to_va(rproc, VEL_APERTURE_SIZE, 1, NULL)` rend bien `NULL`.

Le `heap` n'est donc accessible depuis l'hôte que par la fenêtre glissante, sous verrou
(§3.1), et ce chemin a un consommateur réel : `VEL_IOC_PEEK` (§10.2).

### 6.3 Table de ressources

**Trois** entrées, dans l'ELF :

- **`RSC_CARVEOUT`** — nom `"heap"`, `da = VEL_APERTURE_SIZE`,
  `len = VEL_MEM_SIZE - VEL_APERTURE_SIZE`.
- **`RSC_VDEV` #0** — `VIRTIO_ID_RPMSG` (7), **deux** vrings de `VEL_VRING_NUM`
  descripteurs, `align = VEL_VRING_ALIGN`, `da = FW_RSC_ADDR_ANY`,
  `dfeatures = 1 << VIRTIO_RPMSG_F_NS`.
- **`RSC_VDEV` #1** — `VIRTIO_ID_VELOCITOR`, **deux** vrings de mêmes caractéristiques :
  `engineq0`, `engineq1`. `dfeatures` en v1 : voir §8.1.

Pas de `RSC_TRACE` : voir §6.6.

**Aucun des deux vdev n'a d'espace de configuration virtio.** L'espace de config de
`remoteproc_virtio` vit à la suite du tableau de vrings *dans la table de ressources* ; il
serait donc lisible par le device via la table fantôme, mais le mélanger au plan de contrôle
rpmsg créerait un troisième canal de configuration pour rien. Les capacités passent par
`INFO` (§7).

> **Le nom du carveout est contractuel.** Le core cherche un carveout pré-enregistré par
> son nom, puis vérifie que `da` et `len` concordent. S'il n'en trouve pas, il en crée un
> avec son allocateur DMA générique — c'est-à-dire en mémoire **hôte**. Pour le `heap`,
> c'est exactement ce qu'on veut éviter ; pour les vrings, c'est le comportement qu'on
> reproduit nous-mêmes, en le contrôlant.

> **Limite dure du transport.** `remoteproc_virtio` plafonne à deux vrings par vdev — le
> tableau `vring[]` de `struct rproc_vdev` est dimensionné par `RVDEV_NUM_VRINGS`, qui vaut
> 2. Un `RSC_VDEV` qui en annonce davantage est rejeté. D'où deux vdev de deux vrings, et
> non un vdev de quatre — et d'où le plafond de **deux streams** en v1 (§8.4).

### 6.4 Table fantôme — la seule vue du device sur la négociation

Toutes les `config_ops` de `remoteproc_virtio` — `get_status`, `set_status`, `reset`,
`get_features`, `finalize_features` — lisent et écrivent à travers
`rproc->table_ptr + rvdev->rsc_offset`. Sans `find_loaded_rsc_table`, `table_ptr` reste la
`cached_table`, qui est de la mémoire noyau ordinaire : **le device n'y a aucun accès.**

Conséquence, si on s'en tient là : le modèle ignore quelles features ont été acceptées. Ce
n'est pas cosmétique — `VIRTIO_RING_F_EVENT_IDX` change les règles de notification et
`VIRTIO_RING_F_INDIRECT_DESC` change l'analyse des descripteurs. Un modèle qui les ignore ne
peut pas parser l'anneau correctement, et l'étape 11 du §13 n'a pas d'objet.

**Mécanisme.** Le driver alloue avec `dma_alloc_coherent()` un tampon de `rproc->table_sz`
octets et le retourne depuis son `find_loaded_rsc_table`. `rproc_start()` y recopie
`cached_table` par un `memcpy()` — légitime, puisqu'il s'agit de RAM et non de `__iomem` —
puis fait pointer `table_ptr` dessus. Toutes les écritures ultérieures du core y atterrissent.
Le driver publie l'adresse bus dans `RSC_ADDR_*` et lève `RSC_VALID` avant de relâcher
`RESET`.

Chronologie, à ne pas se représenter à l'envers :

| Moment | Ce qui est écrit dans la table |
|---|---|
| `rproc_handle_resources` | `notifyid` de chaque vring, par `rproc_alloc_vring()` — dans `cached_table` |
| `find_loaded_rsc_table` puis `memcpy` | tout ce qui précède est recopié dans la table fantôme |
| `ops->start()` | le driver publie `RSC_ADDR_*` / `RSC_VALID` ; le device peut lire |
| après `ops->start()`, subdevices | `gfeatures` et statut virtio, écrits directement dans la table fantôme |

Le device lit donc les `notifyid` dès `RSC_VALID`, mais **`gfeatures` seulement à partir de
`VQ_ENABLE`** — ce qui coïncide exactement avec le moment où il en a besoin.

> **Ce que la v0.6.2 avait raison de refuser, et ce qu'elle a refusé de trop.** Placer cette
> table en BAR2 était une erreur : `rproc_start()` la remplirait par un `memcpy()` ordinaire
> vers du `__iomem`. La supprimer entièrement en était une autre, et elle a coûté la
> visibilité de la négociation. En mémoire hôte cohérente, les deux problèmes disparaissent.

### 6.5 Génération et récupération après crash

À chaque passage en `FW_STATUS = 2`, le device incrémente `GENERATION` (§4.1). L'allocateur
repart de zéro : **le handle 7 de la génération 3 n'a rien à voir avec le handle 7 de la
génération 2.**

Sans précaution, un `DeviceBuffer` survivant à un crash redeviendrait silencieusement valide
pour la mauvaise allocation — un ABA classique, et le genre de défaut que ce projet existe
précisément pour rendre visible.

**Tout identifiant exposé au-dessus du driver est donc un couple `{generation, handle}`**, et
tout jeton d'opération un couple `{generation, seq}` (§10.2). Toute opération portant une
génération périmée échoue avec `-ESTALE` et `ERR_CODE = 9`.

**La génération est portée par la réponse, pas déduite après coup.** `vel_alloc_resp` contient
la génération sous laquelle l'allocation a été faite (§7). Composer le couple en relisant
`GENERATION` après la réponse laisserait une fenêtre — étroite, mais déclenchable à volonté
par le bit 2 d'`ERR_INJECT` — où un handle de la génération *n* s'apparie à la génération
*n+1*. C'est le même ABA, déplacé d'un cran.

Séquence de récupération, à spécifier des deux côtés :

1. le device passe en `FW_STATUS = 3` et lève le vecteur 5 ; le modèle annule tout travail
   différé et remet tous les `VQ_ENABLE` à zéro (annexe D) ;
2. le driver marque le contexte comme périmé, appelle `rproc_report_crash()` et fait échouer
   immédiatement toutes les requêtes en vol avec `-ESTALE` ; tous les attendeurs sont
   réveillés ;
3. remoteproc arrête, recharge, redémarre — les sous-drivers sont détruits dans l'ordre
   inverse de leur construction : data, puis ctrl, puis rproc ;
4. **`ops->start()` reprogramme intégralement le transport** : anneaux remis à zéro,
   `notifyid` relus dans la table fantôme, fenêtre `VQ_*` réécrite, `VQ_ENABLE` relevé (§5,
   étape 12). Rien de ce que la génération précédente avait programmé ne survit — le point 1
   l'a effacé côté device, et `rproc_boot_recovery()` ne rejoue ni `prepare` ni le code qui
   suit `rproc_boot()`. C'est ce qui impose de placer la programmation dans `start` ;
5. `GENERATION` s'incrémente, les vdev sont recréés, l'UAPI repasse à `READY` ;
6. les `DeviceBuffer` de l'ancienne génération restent constructibles mais toute opération
   sur eux retourne `-ESTALE` — ils ne mentent pas, ils échouent proprement.

Le char device n'est jamais détruit pendant cette séquence : `VEL_IOC_INFO` rend
successivement `READY`, `CRASHED`, `BOOTING`, `READY`.

### 6.6 En-tête firmware et tampon de trace

**En-tête.** L'ELF porte, à `VEL_FW_HDR_DA`, une structure vérifiée par le modèle au
relâchement de `RESET` :

```c
struct vel_fw_hdr {
    __le32 magic;        /* VEL_FW_MAGIC                         */
    __le32 abi;          /* VEL_FW_ABI                           */
    __le32 trace_da;     /* position du tampon de trace          */
    __le32 trace_len;    /* VEL_TRACE_SIZE                       */
};
```

Sans elle, le chargement serait cérémoniel : le modèle exécute du C qui ne dépend pas des
octets chargés, donc l'étape 6 réussirait avec un `load` entièrement faux. Le magic rend le
chargement falsifiable, et donne enfin un acteur à `FW_STATUS = 1`.

**Trace.** Le mécanisme générique de remoteproc lit le tampon avec un `strnlen()` et rend du
texte via son debugfs : il attend une chaîne bornée par NUL, pas un anneau binaire. Notre
format est structuré, donc **`RSC_TRACE` n'est pas utilisé** — le driver Velocitor expose le
tampon lui-même dans son propre debugfs (§11).

On y gagne : horodatage, appartenance moteur, corrélation par `seq`, et comptage explicite
des entrées perdues.

Le tampon fait `VEL_TRACE_SIZE` et vit dans l'aperture fixe. Anneau de `VEL_TRACE_ENTRIES`
entrées de `VEL_TRACE_ENTRY` octets, précédé d'un en-tête :

```c
struct vel_trace_hdr {
    __le32 head, tail;
    __le32 dropped;      /* entrées écrasées, jamais silencieusement */
    __le32 entry_size;
};

struct vel_trace_entry {
    __le64 timestamp;    /* cycles simulés depuis le reset          */
    __le16 level;
    __le16 engine;       /* 0xFFFF = pas d'appartenance moteur      */
    __le32 seq;
    char   msg[112];
};
```

**Propriété des index.** Le modèle écrit l'entrée complète, *puis* publie `head` ; il
n'écrit jamais `tail`. Le driver est le **seul** propriétaire de `tail`, et il est
sérialisé : une lecture debugfs déclenche une copie des entrées non lues vers un tampon
noyau — avec les primitives d'accès `__iomem` appropriées — puis avance `tail` une fois. Ce
que debugfs rend est cet instantané.

Sans cette règle, deux `cat` concurrents se voleraient les entrées et `dropped` mentirait
sur la cause. L'écrasement silencieux est proscrit : `dropped` est incrémenté par le modèle.

**Sémantique des index.** `head`, `tail` et `dropped` sont des **compteurs libres** sur
32 bits : ils comptent des entrées depuis le démarrage et ne sont jamais repliés sur la
taille de l'anneau. La case d'une entrée est `index % VEL_TRACE_ENTRIES`.

| Grandeur | Expression | Remarque |
|---|---|---|
| anneau vide | `head == tail` | |
| entrées à lire | `head - tail` | soustraction **non signée**, le repli à 2³² se gère seul |
| entrées perdues | `head - tail - VEL_TRACE_ENTRIES`, si positif | corroboré par `dropped` |

Des indices déjà repliés obligeraient à inventer une convention pour distinguer « plein » de
« vide » — sacrifier une case, ou porter un drapeau — et donneraient aux deux implémentations
une occasion de la lire à l'envers. Des compteurs libres n'en ont pas besoin, et c'est la
forme que virtio donne à `avail->idx` pour la même raison.

**Le driver doit se resynchroniser avant de copier.** Si `head - tail > VEL_TRACE_ENTRIES`,
les entrées les plus anciennes ont déjà été écrasées : `tail` désigne une case dont le
contenu ne lui appartient plus. Il pose alors `tail = head - VEL_TRACE_ENTRIES` et rapporte
l'écart. Copier depuis l'ancien `tail` rendrait des entrées récentes en les présentant comme
anciennes — un mensonge que `dropped` ne rattraperait pas, puisqu'il compte ce qui a été
écrasé, pas ce qui a été mal lu.

L'ordre de lecture est le miroir de l'ordre de publication : le driver lit `head` d'abord,
les entrées ensuite. Le modèle écrit l'entrée d'abord, `head` ensuite.

---

## 7. Plan de contrôle — rpmsg

Service `velocitor-ctrl`, adresse `VEL_RPMSG_CTRL_ADDR`. Requête/réponse appariées par `seq`.

### 7.1 Le modèle est un endpoint rpmsg

Le §7.2 décrit le *payload* applicatif. Il ne va pas de soi, pour celui qui écrit le modèle,
que tout un transport le précède : le faux firmware doit consommer les *split rings*, gérer
les tampons de réception fournis par Linux, construire l'en-tête `rpmsg_hdr`, et respecter
les adresses source et destination — le format filaire de `virtio_rpmsg_bus` est repris tel
quel, sans variante.

> **Le modèle doit émettre l'annonce *name service*.** Linux ne crée un `rpmsg_device`
> dynamique que si `VIRTIO_RPMSG_F_NS` est négocié **et** que le distant envoie une annonce
> sur `VEL_RPMSG_NS_ADDR`. Sans cela, aucun canal n'apparaît côté hôte et rien ne se bind —
> piège classique, qui coûte une soirée à celui qui l'ignore.
>
> **Le moment est contraint**, et pas comme la v0.6.3 le croyait. Elle disait « quand les
> deux vrings de vdev0 sont `VQ_ENABLE` », ce qui valait tant que le driver programmait la
> fenêtre au retour de `rproc_boot()`. Depuis que le §5 la place dans `ops->start()`,
> l'activation précède le démarrage des sous-périphériques : à cet instant `gfeatures` est
> encore à zéro, donc `VIRTIO_RPMSG_F_NS` n'y figure pas et la condition ci-dessous
> l'interdit elle-même.
>
> Le déclencheur est donc le **premier doorbell où `F_NS` est négocié et où un tampon de
> réception est disponible** — c'est-à-dire le `virtqueue_kick()` que `virtio_rpmsg_bus`
> émet à son *probe* après avoir rempli l'anneau, et le premier instant où l'annonce peut
> physiquement partir.
>
> **Le contenu est contraint aussi** : `{ name = VEL_CTRL_NAME, addr = VEL_RPMSG_CTRL_ADDR,
> flags = NS_CREATE }`, au format `struct rpmsg_ns_msg`. C'est le seul message que les deux
> implémentations doivent produire et consommer **avant** de disposer d'un canal pour se
> coordonner : s'il diverge, rien en amont ne permet de le diagnostiquer.

Le modèle ne doit émettre l'annonce que si `VIRTIO_RPMSG_F_NS` figure dans les `gfeatures`
de vdev0, lus dans la table fantôme (§6.4).

### 7.2 Messages

```c
struct vel_msg {
    __le32 seq;
    __le16 op;
    __le16 flags;
    __le32 status;     /* réponse uniquement, 0 = OK, négatif = -errno */
    __le32 reserved;   /* écrit à zéro, ignoré en lecture */
    __u8   payload[];
};
```

| `op` | Nom | Description |
|---:|---|---|
| 1 | `INFO` | capacités du firmware, topologie, alignement requis |
| 2 | `ALLOC` | réserve un bloc sur un nœud donné, retourne un handle |
| 3 | `FREE` | **invalide** le handle ; la mémoire peut ne pas être récupérée |
| 4 | `STAT` | capacité et mémoire libre par nœud, handles vivants |

```c
struct vel_info_resp {
    __le32 abi;             /* VEL_FW_ABI                              */
    __le32 caps;            /* ce que le firmware croit du matériel    */
    __le32 ctrl_caps;       /* bit0 : respecte le nœud demandé         */
    __le32 nodes, engines;
    __le32 alloc_align;
    __le32 generation;
};

struct vel_alloc_req  { __le64 size; __le32 dtype; __le32 node; };
struct vel_alloc_resp { __le32 handle; __le32 node;
                        __le32 generation; __le32 reserved;
                        __le64 dev_offset; };

struct vel_free_req   { __le32 handle; __le32 reserved; };

struct vel_stat_node  { __le64 capacity; __le64 free; };
struct vel_stat_resp  { struct vel_stat_node node[VEL_NODES];
                        __le32 live_handles; __le32 reserved; };
```

`node` vaut 0, 1, ou `VEL_NODE_ANY`. C'est le point de décision : allouer près du moteur
qui consommera, ou laisser faire et le mesurer.

**Le respect du nœud demandé est une capacité du plan de contrôle**, `ctrl_caps` bit 0 — et
non une feature du vdev de données, où elle se trouvait jusqu'en v0.6.2. Une capacité qui
gouverne `ALLOC` n'a rien à faire dans la négociation d'un autre plan.

**`STAT` rend la capacité *et* le libre par nœud**, parce que les deux nœuds n'ont pas la
même taille allouable (§3.2) et qu'un protocole de mesure qui l'ignore compare deux choses
différentes.

**Les handles commencent à 1.** Zéro est le sentinelle « matrice non résidente » du §8.3 :
un allocateur *bump* qui rendrait 0 à la première allocation ferait traiter celle-ci comme
une adresse hôte, silencieusement, une fois par session.

**`FREE` porte le handle qu'il invalide**, dans un `vel_free_req`. La v0.6.3 décrivait
l'opération sans lui donner de requête — le tableau des `op` la nomme, les structures ne la
définissent pas. `flags` et `reserved` de `vel_msg` ne pouvaient pas la porter : le §7.2 dit
`reserved` écrit à zéro et ignoré en lecture, et détourner un champ existant est exactement
ce que le §10.2 interdit pour l'UAPI. Ajoutée à l'implémentation, cf. §16.

**`FREE` invalide toujours le handle**, même si l'allocateur ne récupère pas la mémoire.
Sans cela, l'erreur `ERR_CODE = 3` serait indétectable. Les handles ne sont jamais
réutilisés dans une même session de firmware.

### 7.3 Diagnostic croisé `CAPS` / `INFO`

`CAPS` (§4.1) décrit le matériel ; le champ `caps` d'`INFO` décrit ce que le firmware croit
du matériel. Le doublon est délibéré : au *bring-up*, le driver lit `CAPS`, demande `INFO`,
et **journalise tout écart dans `mismatch`** (§11).

C'est le seul endroit du document où le device peut se contredire lui-même de façon
détectable, et c'est exactement le genre de preuve que le §0.3 réclame. Un écart ne bloque
pas le démarrage ; il est signalé.

---

## 8. Plan de données — virtio

Device virtio propre, **une queue par moteur** : `engineq0`, `engineq1`.

### 8.1 Bits de fonctionnalités

> **Deux exclusions imposées par le transport remoteproc, à ne pas contourner :**
> `VIRTIO_F_VERSION_1` est le bit 32, or `rproc_virtio_finalize_features` contient un
> `BUG_ON` interdisant toute feature au-delà de 32 bits — le champ `dfeatures` de la table
> de ressources est un `u32`. Et `rproc_transport_features` efface systématiquement
> `VIRTIO_F_RING_PACKED`, remoteproc utilisant `vring_new_virtqueue()` sur mémoire
> préallouée. Donc : **legacy split rings, features sur 32 bits.**

**En v1, `dfeatures` du vdev de données vaut zéro.** Le *bring-up* de l'étape 8 se fait en
*split ring* nu : chaînes par `NEXT`, buffers en lecture et en écriture, aucune option.

Les features ne sont introduites qu'à l'**étape 11**, une par une, et chacune change quelque
chose de réel :

| Bit | Nom | Ce que ça change |
|---:|---|---|
| 0 | `VEL_F_BF16` | dtype accepté par `GEMM` |
| 1 | `VEL_F_TRANSPOSE` | bits 0 et 1 de `vel_gemm_hdr.flags` honorés |
| 2 | `VEL_F_SG` | **réservé — v1.1**, cf. §14 |
| 3 | `VEL_F_STEAL` | **réservé — hors v1**, cf. §8.4 |
| 28 | `VIRTIO_RING_F_INDIRECT_DESC` | table de descripteurs secondaire |
| 29 | `VIRTIO_RING_F_EVENT_IDX` | règles de notification |

`VIRTIO_RING_F_INDIRECT_DESC` n'apportera **jamais** rien à une chaîne de deux descripteurs
(§8.3). Il est négocié à l'étape 11 comme exercice de négociation et de parsing, pas comme
optimisation : personne ne doit en attendre une amélioration mesurable, et le rapport ne doit
pas en revendiquer une.

Les bits réservés ne sont pas négociés mais restent déclarés, pour ne pas renuméroter plus
tard.

**Le modèle lit `gfeatures` dans la table fantôme (§6.4), jamais `dfeatures`.** Confondre les
deux, c'est parser l'anneau selon ce qui a été offert et non selon ce qui a été accepté.

### 8.2 Opérations

Le plan de données porte trois opérations, toutes soumises de la même façon :

| `op` | Nom | Effet |
|---:|---|---|
| 1 | `COPY_H2D` | remplit un `DeviceBuffer` depuis la mémoire hôte |
| 2 | `COPY_D2H` | rapatrie un `DeviceBuffer` vers la mémoire hôte |
| 3 | `GEMM` | calcul, opérandes résidents ou lus directement en mémoire hôte |

> **Pourquoi des copies explicites.** La mémoire hôte projetée par `mmap` et la mémoire
> locale du device sont deux espaces distincts. Sans `COPY_H2D`, `alloc()` puis écriture
> côté hôte puis `GEMM` sur le handle ne transfère rien — c'était le défaut de la v0.5.

Ces copies vivent sur le vdev de données, **pas** en rpmsg : c'est du volume. Et comme elles
partagent la queue avec `GEMM`, l'ordre de soumission donne une sémantique de flux.

**Sémantique moteur d'une copie.** Une commande appartient à une queue, donc à un moteur. Le
*bus-mastering* est une capacité commune du device, mais l'accès à la mémoire locale est
compté au moteur de la queue : une `COPY_H2D` vers un `DeviceBuffer` du nœud distant subit
`VEL_FAR_PENALTY` et incrémente `CNT_FAR_ACCESS`, et `vel_resp.far_accesses` peut donc être
non nul pour une copie. La traversée PCIe, elle, n'est jamais pénalisée — l'annexe A.6
exclut déjà toute simulation de contention de lien.

### 8.3 Chaîne de descripteurs

En v1, la virtqueue ne porte que les **petites structures de commande et de réponse**. Les
données restent dans le tampon cohérent, et leur adresse bus voyage dans la commande.

| # | Sens | Contenu |
|---:|---|---|
| 0 | lecture | `struct vel_req_hdr` + en-tête d'opération |
| 1 | écriture | `struct vel_resp` |

Soumission par `virtqueue_add_sgs()` avec ces deux éléments, puis `virtqueue_kick()`.
Complétion par `virtqueue_get_buf()`.

> **Pourquoi les données ne sont pas dans la chaîne en v1.** Le tampon de l'application vient
> de `dma_alloc_coherent()` : il possède déjà une adresse bus stable et relève du modèle DMA
> *cohérent*. Le mettre dans une scatterlist le ferait remapper comme un tampon *streaming* —
> deux modèles DMA différents, mélangés pour rien. Le device lit et écrit donc directement à
> l'adresse fournie, en bus-master.
>
> **En v1.1**, avec `pin_user_pages()` et de vraies scatterlists issues de pointeurs
> utilisateur, les données rejoignent la chaîne et `VEL_F_SG` devient réellement démontré.

Toutes les structures partagées avec le device sont en types explicitement *little-endian* —
pas seulement celles où la question s'est posée. Voir annexe A.1.

```c
struct vel_host_range {
    __le64 dma_addr;       /* adresse bus, dans le tampon cohérent    */
    __le64 len;
};

struct vel_req_hdr {
    __le32 seq;
    __le32 generation;     /* rejeté avec -ESTALE si périmée, cf. §6.5 */
    __le16 op;             /* COPY_H2D | COPY_D2H | GEMM               */
    __le16 flags;
    __le32 reserved;
};

struct vel_copy_hdr {      /* suit vel_req_hdr pour COPY_*             */
    __le32 handle;
    __le32 reserved;
    __le64 dev_offset;     /* position dans le DeviceBuffer            */
    struct vel_host_range host;
};

struct vel_gemm_hdr {      /* suit vel_req_hdr pour GEMM               */
    __le32 h_a, h_b, h_c;  /* 0 = matrice non résidente, cf. host[]    */
    __le32 m, n, k;
    __le32 dtype;
    __le32 flags;          /* bit0 transpose A, bit1 transpose B       */
    struct vel_host_range host[3];   /* utilisé pour chaque handle nul */
};

struct vel_resp {
    __le32 seq;
    __le32 status;
    __le64 cycles;
    __le32 far_accesses;   /* accès au nœud distant pendant l'opération */
    __le32 engine;         /* moteur ayant traité ; = queue en v1       */
};
```

Un handle à zéro signifie que la matrice n'est pas résidente : le device la lit ou l'écrit
directement dans le tampon cohérent, à l'adresse bus donnée par l'entrée `host[]`
correspondante. C'est utile pour un calcul unique dont les opérandes ne serviront qu'une
fois — pour tout le reste, la résidence via `COPY_H2D` évite de retraverser le lien.

`vel_resp.engine` est **redondant en v1** : sans vol de travail, le moteur effectif est
toujours celui de la queue. Le champ est conservé pour ne pas modifier la structure plus
tard.

**En v1.1**, ces `vel_host_range` sont remplacées par de vraies scatterlists issues de
pointeurs utilisateur pincés, et rejoignent la chaîne de descripteurs.

**Sémantique GEMM :** `C = A × B`, *row-major*, `A` est m×k, `B` est k×n, `C` est m×n.
**L'accumulation se fait toujours en fp32**, y compris pour des entrées bf16 ; le résultat est
arrondi au dtype de `C` à l'écriture. L'ordre de réduction n'est pas spécifié.

**L'aliasing entre A, B et C est interdit en v1** et rejeté avec `ERR_CODE = 5` : le définir
demanderait une sémantique d'ordre lecture/écriture, l'interdire coûte une comparaison.

> **Oracle et tolérance.** Un oracle calculé sur l'hôte ne donnera pas un résultat bit à bit
> identique, et c'est délibéré. Le critère est
> `|got − ref| ≤ atol + rtol · |ref|`, avec `atol` et `rtol` fixés par dtype dans l'en-tête
> partagé, et **l'erreur maximale observée est enregistrée par la CI**. « Une tolérance à
> justifier » est une intention ; ceci est un critère. La distinction entre variation
> numérique légitime et corruption réelle ne s'établit pas autrement.

### 8.4 Ordre d'exécution et streams

Règle unique, et elle est plus stricte que « FIFO par queue » :

> **Le device n'exécute qu'une commande à la fois par queue, dans l'ordre de soumission.
> Aucune garantie d'ordre entre `engineq0` et `engineq1`.**

Cela ne limite pas le nombre de commandes **postées** : l'hôte peut en soumettre autant que
la virtqueue en accepte, et en attendre plusieurs simultanément. C'est le device qui ne
démarre la suivante qu'après avoir terminé la précédente.

C'est ce qui donne la sémantique de flux : sur une même queue, `COPY_H2D` puis `GEMM` puis
`COPY_D2H` s'enchaînent correctement sans aucune fence.

Les complétions des deux queues peuvent revenir dans n'importe quel ordre entre elles ;
jamais à l'intérieur d'une même queue.

**Un stream est une queue.** Le runtime expose cet ordre sous la forme d'un objet `Stream`
(§10.3) : toute opération soumise sur le même stream part sur la même virtqueue, donc sur le
même moteur, donc avec la dépendance implicite ci-dessus. Sans cet objet, la sémantique de
flux serait une convention non exprimable depuis l'application, et le placement sur nœud —
le sujet mesurable du projet — ne serait pas décidable.

**Il y a exactement `VEL_STREAMS` = 2 streams en v1**, et ce n'est pas un choix : le
transport remoteproc plafonne à deux vrings par vdev (§6.3). Au-delà, il faut un vdev
supplémentaire. L'API doit donc l'énoncer plutôt que suggérer une extensibilité qu'elle n'a
pas.

#### Conséquence : `VEL_F_STEAL` est hors périmètre

Cette règle vide le vol de son intérêt, et il faut le dire franchement. Soit `engineq0`
portant `A`, `B`, `C`, le moteur 0 occupé par `A`, le moteur 1 oisif. Le vol consisterait à
faire prendre `B` par le moteur 1 — mais cela ferait deux commandes en exécution sur
`engineq0`, que la règle interdit. Le moteur 1 attend donc la fin de `A` ; à ce moment le
moteur 0 est libre de lui-même, et le vol n'apporte plus rien.

Autrement dit : le cas de déséquilibre que la fonctionnalité devait traiter disparaît avec
la contrainte qui garantit la sémantique de flux. On ne peut pas avoir les deux sans
introduire des dépendances explicites — ce qui serait une conception nouvelle, pas un
errata.

**Décision : `VEL_F_STEAL` sort du périmètre v1.** Le bit reste réservé. Le choix se fera
après la chaîne minimale, sur mesure et non sur papier, entre deux options :

| Option | Idée | Coût |
|---|---|---|
| Ordonnancement au niveau des flux | plus de streams strictement séquentiels ; le parallélisme vient du nombre de flux — modèle des *streams* CUDA | un vdev de plus |
| Plusieurs commandes en exécution par queue | dépendances ou fences explicites dans le protocole | protocole nettement plus lourd |

Ce que le §12 item 5 mesure, en attendant, est exactement le coût de cette absence.

---

## 9. Méchanceté délibérée

> **Statut de cette section.** Ces comportements sont des **artefacts de modèle** destinés
> à rendre obligatoires des idiomes corrects. Ils s'inspirent de modes de défaillance réels
> sans prétendre les reproduire littéralement. Le rapport final doit le dire.

> **Tout est déterministe.** Aucun tirage aléatoire, nulle part. L'item 6 du §12 demande le
> comportement sous *chaque* injection : sans reproductibilité, ce n'est pas une mesure mais
> une anecdote, et la CI du §13.1 ne peut rien en faire.

Comportements permanents :

- **Masque DMA de 42 bits, testé par excès.** Linux part d'un masque à 32 bits ; un driver
  qui n'appelle jamais `dma_set_mask_and_coherent` recevrait des adresses basses,
  parfaitement valides — le device ne peut pas le détecter. Le piège porte donc sur le cas
  inverse : **si un IOVA dépasse 42 bits, le device rejette le transfert** avec
  `ERR_CODE = 4`.
- **`WIN_BASE` exige un *read-back*.** Modèle artificiel : le device ne prend la nouvelle
  base en compte qu'après une relecture du registre. La relecture est l'idiome réel pour
  vider les tampons d'écriture ; ici on le rend obligatoire.
- **`WIN_BASE` est partagé**, sans sérialisation matérielle.
- **Notifications coalescées, de façon déterministe.** Tant qu'un réveil est en attente, un
  second doorbell n'en crée pas un autre ; au réveil, le device rebalaie l'anneau.
  `CNT_NOTIFY_COALESCED` compte les fusions, ce qui rend l'écart prévisible et donc
  soustractible (§4.5). L'invariant à retenir : l'anneau est la source de vérité, la
  notification n'est qu'un signal.
- **Pas de barrière implicite** sur les structures hors virtio écrites en mémoire locale.
- **Le coût d'un accès distant n'est jamais signalé.** Il n'apparaît que dans
  `CNT_FAR_ACCESS` et dans `far_accesses`.

`ERR_INJECT` déclenche les cas d'erreur à la demande. `ERR_INJECT_ARG` paramètre celle qui
est active :

| Bit | Effet | `ERR_INJECT_ARG` |
|---:|---|---|
| 0 | le prochain GEMM retourne `-EIO` | — |
| 1 | corruption d'un octet tous les *N* octets transférés, compteur remis à zéro à l'armement | *N*, défaut 1000 |
| 2 | le firmware passe en `FW_STATUS = 3` et lève le vecteur 5 | — |
| 3 | la prochaine complétion est retardée | délai en millisecondes de temps virtuel, défaut 5000 |
| 4 | la prochaine notification de queue est supprimée (`CNT_NOTIFY_TX` s'incrémente quand même) | — |
| 5 | le prochain `ALLOC` retourne `-ENOMEM` quelle que soit la mémoire libre | — |
| 6 | `WIN_BASE` ignore silencieusement la prochaine écriture | — |
| 7 | le moteur 1 cesse de consommer sa queue | — |
| 8 | la prochaine opération est traitée comme portant une génération périmée | — |

Le bit 2 exerce la récupération remoteproc : arrêt, rechargement, redémarrage, invalidation
de tous les handles. Le bit 7 crée un déséquilibre que seuls les compteurs révèlent. Le
bit 3 s'appuie sur un timer virtuel, jamais sur du temps mur, pour que la CI ne paie pas
cinq secondes réelles.

### 9.1 Provoquer le piège des 42 bits

Un driver **correct** fixe son masque à 42 bits et ne recevra donc jamais d'IOVA au-delà :
le piège ne serait jamais exercé par le chemin nominal, et resterait décoratif.

Deux conditions sont donc nécessaires :

- **côté VM** : une configuration produisant des IOVA hauts — vIOMMU avec largeur d'adresse
  configurable, ou disposition mémoire adéquate. **À trancher**, cf. §14 ;
- **côté driver** : un paramètre de module explicitement fautif, `dma_bits_override=64`, qui
  annonce un masque plus large que le matériel. C'est lui qui rend le piège **testable**
  plutôt qu'hypothétique, et c'est un cas de test de la couche 2 du §13.1.

Le §12 item 6 doit rendre compte de ce que le driver fait de `ERR_CODE = 4` — pas seulement
du fait que le device l'a levé.

---

## 10. Composition du driver et runtime utilisateur

La moitié du sujet. Un driver sans consommateur ne démontre rien.

### 10.1 Composition côté noyau

Ce n'est pas un module monolithique : ce sont **trois drivers sur trois bus différents**,
avec des probes et des durées de vie séparés.

```
velocitor_pci                     (bus PCI)
   │
   ├── état commun  vel_dev        cartes BAR, MSI-X, compteurs, génération,
   │                               pool hôte, vrings, table fantôme
   │
   ├── rproc                       remoteproc : ELF, carveouts, load/start
   │      ├── rpmsg device  "velocitor-ctrl"  → velocitor_ctrl_driver  (bus rpmsg)
   │      └── virtio device  vdev1            → velocitor_data_driver  (bus virtio)
   │
   └── /dev/velocitor              misc device, enregistré au probe PCI ;
                                   n'accepte les requêtes qu'à l'état READY
```

Conséquences à respecter :

- Le char device est enregistré au `probe` PCI et **refuse toute opération avec `-ENODEV`
  tant que les deux sous-drivers ne sont pas prêts.** Il survit aux crashes et aux reprises :
  l'application ne perd jamais son descripteur, ce qui est la condition pour observer la
  reprise depuis l'application (§12 item 6).
- À l'arrêt du firmware, l'ordre de destruction est l'inverse de la construction : data,
  puis ctrl, puis rproc. Les requêtes en vol échouent avec `-ESTALE` avant toute libération,
  et tous les attendeurs sont réveillés.
- Le pool hôte, les vrings et la table fantôme appartiennent à `velocitor_pci` et survivent
  aux cycles remoteproc.
- Après récupération, les sous-drivers sont recréés et la génération a changé (§6.5).

`misc_register` impose un nom fixe : **une seule carte est supportée**, et un second `probe`
échoue proprement. La règle d'ouverture exclusive (§10.2) rend cette limite sans conséquence
pour le projet.

### 10.2 UAPI — `/dev/velocitor`

Char device (`misc_register`). En-tête `uapi/velocitor.h`, partagé avec la bibliothèque.

| ioctl | Effet |
|---|---|
| `VEL_IOC_INFO` | topologie, capacités, features négociées, alignement, génération, **état** |
| `VEL_IOC_ALLOC_HOST` | alloue une plage dans le pool cohérent, retourne un identifiant + offset |
| `VEL_IOC_FREE_HOST` | libère cette plage |
| `VEL_IOC_ALLOC_DEV` | alloue en mémoire locale, retourne `{generation, handle}` + nœud |
| `VEL_IOC_FREE_DEV` | libère un handle device |
| `VEL_IOC_COPY` | `COPY_H2D` ou `COPY_D2H` sur un stream ; retourne un jeton |
| `VEL_IOC_SUBMIT` | soumet un GEMM sur un stream ; retourne un jeton |
| `VEL_IOC_WAIT` | attend un jeton, avec délai maximal ; retourne le résultat |
| `VEL_IOC_PEEK` | lit une plage d'un `DeviceBuffer` via la fenêtre glissante — diagnostic |
| `VEL_IOC_STATS` | compteurs device et driver, pour l'outil de mesure |

`VEL_IOC_INFO` rend un état parmi `DOWN`, `BOOTING`, `READY`, `CRASHED`.

`VEL_IOC_PEEK` est lent par construction — verrou sur `WIN_BASE`, *read-back*, copie
fenêtrée — et n'est pas un chemin de données. Il existe pour donner un consommateur réel au
mécanisme de fenêtre, et pour permettre de regarder ce qu'il y a dans un `DeviceBuffer`
quand un résultat est douteux.

`mmap` sur le char device projette le **pool hôte cohérent** alloué par le driver
(`dma_alloc_coherent`, `VEL_HOST_POOL_SIZE`), que l'application remplit directement.

**Aucune adresse DMA ne vient jamais de l'espace utilisateur.** `VEL_IOC_COPY` et
`VEL_IOC_SUBMIT` désignent des plages par `{host_id, offset, len}` ; c'est le noyau qui les
traduit en `vel_host_range` à partir de sa propre allocation. Un `dma_addr_t` fourni par
l'application serait une primitive d'écriture arbitraire en mémoire physique.

Validations obligatoires à l'entrée, toutes en arithmétique protégée du débordement :
`m·k·sizeof(dtype)`, `k·n`, `m·n` contre les tailles des `DeviceBuffer` ; `dev_offset + len`
contre la taille du handle ; appartenance de la plage hôte au pool ; concordance du dtype
avec les features négociées ; génération ; non-aliasing A/B/C.

> **À trancher** : autoriser aussi des pointeurs utilisateur arbitraires via
> `pin_user_pages` + `dma_map_sg`. Plus réaliste, nettement plus de code. Le mapping
> cohérent suffit pour la v1.

Structures d'UAPI : champs de largeur fixe, alignement sur 8, champ de version en tête,
extensions par ajout en fin de structure et jamais par réinterprétation d'un champ existant.
**Pas de contrainte d'endianness ici** — espace utilisateur et noyau sont sur la même
machine ; l'endianness explicite ne concerne que ce qui est partagé avec le device
(annexe A.1).

#### Jetons et durée de vie asynchrone

Un jeton est un couple `{generation, seq}`. Il n'est jamais réutilisé tant qu'il est en vol.

| Cas | Décision v1 |
|---|---|
| Virtqueue pleine à la soumission | `-EAGAIN`, l'appelant réessaie ; pas d'attente dans l'ioctl |
| `WAIT` qui expire | le jeton **n'est pas consommé**, l'opération reste en vol |
| `WAIT` qui réussit | le jeton est consommé ; toute réutilisation rend `-EINVAL` |
| Crash firmware | tous les attendeurs réveillés, tous les jetons en vol rendus `-ESTALE` |
| Plusieurs processus | **un seul `open` à la fois**, `-EBUSY` sur le second |
| Opération portant une génération périmée | `-ESTALE` |

**Les tampons sont comptés par référence, pas verrouillés.** Une requête en vol détient une
référence sur chaque `HostBuffer` et `DeviceBuffer` qu'elle utilise. `VEL_IOC_FREE_DEV` et
`VEL_IOC_FREE_HOST` rendent immédiatement l'objet indisponible à toute nouvelle soumission
et **réussissent toujours** ; la réutilisation effective est différée jusqu'à la dernière
complétion.

C'est la seule règle compatible avec le RAII promis au §10.3 : un destructeur ne peut pas
rattraper un `-EBUSY`, et l'ignorer silencieusement serait pire. Avec le *bump allocator*
du §14, cette règle est presque gratuite — la mémoire n'étant jamais récupérée, le refcount
ne diffère que le retrait de l'entrée de la table des handles.

L'exclusivité d'ouverture supprime beaucoup de plomberie sans rien retirer à la valeur
pédagogique. À rouvrir si un besoin réel apparaît.

### 10.3 `libvelocitor` — C++

Enveloppe mince, sans dépendance, au-dessus de l'UAPI.

**Les deux mémoires sont deux types distincts, et l'ordre est un troisième type.** C'est le
point central : rien ne relie implicitement les deux mémoires, rien ne garantit implicitement
l'ordre, et le typage doit rendre les deux erreurs impossibles à écrire.

```cpp
namespace vel {

class HostBuffer {                       // plage dans la zone mmap
public:
    std::span<std::byte> data();         // accès direct depuis l'application
    size_t size() const;
};                                       // pas de handle : le device ne la connaît pas

class DeviceBuffer {                     // allocation en mémoire locale
public:
    uint32_t handle() const;             // jamais 0
    uint32_t generation() const;
    Node     node() const;
    size_t   size() const;
};                                       // pas de data() : l'hôte ne l'adresse pas

class Stream {                           // une queue de moteur ; ordre garanti
public:
    Engine engine() const;

    Future<Result> copy(DeviceBuffer& dst, const HostBuffer& src);   // H2D
    Future<Result> copy(HostBuffer& dst, const DeviceBuffer& src);   // D2H
    Future<Result> submit(const GemmDesc& d);
};

class Device {
public:
    explicit Device(const char* path = "/dev/velocitor");
    Info info() const;

    Stream&      stream(unsigned i);     // i < VEL_STREAMS, soit 2 en v1
    HostBuffer   alloc_host(size_t bytes);
    DeviceBuffer alloc_device(size_t bytes, DType dt, Node node = Node::Any);

    Stats stats() const;
};

}
```

`HostBuffer` n'a pas de `handle()`, `DeviceBuffer` n'a pas de `data()`. Le compilateur
interdit donc l'erreur de la v0.5 — remplir un tampon hôte puis soumettre un handle device
en croyant avoir transféré quelque chose.

Il n'y a pas de `Device::submit()` : toute opération part d'un `Stream`, donc d'une queue
connue. La séquence `s.copy(...); s.submit(...); s.copy(...)` a la sémantique de flux du
§8.4 par construction, sans que l'appelant ait à le savoir.

Points à démontrer : durée de vie par RAII des deux côtés, plusieurs opérations en vol,
chaînage copie → calcul → copie sans aller-retour hôte, placement explicite sur les deux
streams, et reprise après crash firmware — un `DeviceBuffer` d'une génération périmée reste
constructible mais toute opération sur lui retourne `-ESTALE` (§6.5). Il échoue proprement,
il ne ment pas.

### 10.4 `velocitor-top` — outil de mesure

Utilitaire en ligne de commande, dans l'esprit des `pcm-*` d'Intel. Trois modes :

- **moniteur** : rafraîchissement continu des compteurs device et driver, profondeur des
  queues, cycles et stalls par moteur, accès distants ;
- **banc** : soumet une charge paramétrée et compare deux configurations — placement proche
  contre distant, charge symétrique contre asymétrique sur les deux streams ;
- **injection** : écrit `ERR_INJECT` et `ERR_INJECT_ARG`, et vérifie la réaction du driver.

Langage libre — c'est le seul composant sans contrainte, et un bon endroit pour du Rust si
l'envie prend.

---

## 11. Observabilité côté hôte

### debugfs — `/sys/kernel/debug/velocitor/<dev>/`

| Entrée | Contenu |
|---|---|
| `state` | `FW_STATUS`, état UAPI, position de la fenêtre, `gfeatures` par vdev, `notifyid` |
| `handles` | handles vivants : taille, nœud, offset, génération, refcount, âge |
| `inflight` | requêtes en vol : jeton, stream, âge, état |
| `counters` | compteurs device, décodés, sur instantané `CNT_SNAP` |
| `engines` | par moteur : cycles, stalls, accès distants, profondeur de queue |
| `trace` | instantané de l'anneau de trace ; le driver seul avance `tail` (§6.6) |
| `mismatch` | écarts entre ce que le driver compte et ce que le device compte, **et** écart `CAPS`/`INFO` (§7.3) |
| `err_inject` | écriture directe de `ERR_INJECT` et `ERR_INJECT_ARG` |

`mismatch` est l'entrée la plus utile : elle transforme un doute en fait. Elle est **exacte à
queue quiescente** et indicative sous charge — un écart observé pendant l'exécution peut
n'être qu'un décalage d'échantillonnage, et le dire évite de transformer le meilleur outil du
projet en générateur de faux positifs.

### Tracepoints — `include/trace/events/velocitor.h`

| Tracepoint | Champs |
|---|---|
| `velocitor_submit` | jeton, stream, `notifyid`, m/n/k, dtype, nb de descripteurs |
| `velocitor_complete` | jeton, statut, cycles, `far_accesses` |
| `velocitor_dma_range` | direction, taille, adresse bus, nb de plages |
| `velocitor_win_move` | ancienne base, nouvelle base, appelant |
| `velocitor_irq` | vecteur, `notifyid` — **aucun champ lu en MMIO**, cf. §3.3 |
| `velocitor_error` | `ERR_CODE`, `ERR_NOTIFYID`, `ERR_HANDLE`, `ERR_DROPPED` |
| `velocitor_fw_state` | ancien état, nouvel état |
| `velocitor_uapi` | ioctl, handle, durée |

`velocitor_dma_range` ne s'appelle pas `dma_map` : en v1 le chemin de données ne mappe rien,
il utilise une allocation cohérente déjà mappée. Un tracepoint nommé d'après une opération
qui n'a pas lieu fausserait précisément les mesures des items 2 et 3 du §12. Il sera
renommé — ou remplacé — en v1.1, avec `pin_user_pages` et `dma_map_sg`.

`velocitor_irq` ne porte que ce que le handler connaît déjà. Lire `IRQ_STATUS` pour le
tracepoint annulerait le bénéfice revendiqué au §3.3 : un aller-retour PCIe par interruption,
réintroduit par l'instrumentation.

Désactivés, les tracepoints sont des NOP patchés : coût nul, activables à chaud, sans
recompilation ni build de debug. Exploitables par ftrace et perf.

---

## 12. Ce que le projet doit produire

Le code n'est pas le livrable.

1. Une chaîne complète et fonctionnelle : application → `libvelocitor` → UAPI → driver →
   remoteproc → rpmsg/virtio → modèle QEMU.
2. Le nombre de descripteurs et d'opérations DMA par GEMM, comparé au minimum théorique —
   `CNT_DESC` contre `CNT_DMA_RD`/`CNT_DMA_WR`, dont le §4.5 fixe la granularité.
3. Le nombre d'accès registre et de barrières par déplacement de fenêtre, mesuré sur
   `VEL_IOC_PEEK`.
4. L'effet du placement sur nœud : même charge, allocation proche contre distante, sur le
   même stream — en tenant compte de l'asymétrie de capacité du §3.2.
5. Le déséquilibre entre moteurs sous charge asymétrique, lu dans `CNT_STALL_E0` et
   `CNT_STALL_E1` — combien de travail est perdu faute de pouvoir le déplacer, et donc ce
   que coûterait vraiment l'absence de vol (§8.4).
6. Le comportement sous chaque injection d'erreur, et ce que le driver en fait — en
   particulier la reprise après crash firmware **vue depuis l'application** : un
   `DeviceBuffer` survivant échoue-t-il en `-ESTALE` ou redevient-il valide pour la mauvaise
   allocation ; et ce que le driver fait de `ERR_CODE = 4` sous `dma_bits_override=64`.
7. Un exemple de diagnostic croisé : une notification perdue — bit 4 d'`ERR_INJECT` —
   identifiée en confrontant `CNT_NOTIFY_TX`, `CNT_NOTIFY_COALESCED` et le compteur
   d'interruptions du driver, via `mismatch`.
8. Une note honnête sur ce que le modèle QEMU ne garantit pas, et donc sur ce que la
   validation en émulation ne prouve pas.

---

## 13. Étapes

| # | Contenu | Sortie |
|---|---|---|
| 0 | Outillage : QEMU 7.2.22 et Linux 6.18.44 **épinglés** et §C.4 exécutée, VM avec CMA, console série, module qui charge | itération en 30 s |
| 1 | En-tête partagé des constantes et structures | contrat matérialisé |
| 2 | `probe`, BAR0, `MAGIC`, `SCRATCH`, compteurs, `CNT_SNAP` | le driver parle au device |
| 3 | MSI-X, handler, `IRQ_ACK`, tracepoint `irq` | interruption reçue et acquittée |
| 4 | BAR2 : aperture fixe, fenêtre glissante, read-back | balayage complet de la mémoire |
| 5 | DMA bus-master nu via `DBG_DMA_*`, hors virtio, vers le pool cohérent | aller-retour vérifié |
| 6 | remoteproc : `auto_boot=false`, `rproc_add`, ELF + en-tête, carveouts, `da_to_va`, table fantôme, trace | `FW_STATUS = 2` |
| 7 | vdev0 : rpmsg, annonce NS, `INFO`, `ALLOC`, `FREE`, `STAT`, croisement `CAPS`/`INFO` | plan de contrôle vivant |
| 8 | vdev1 en split ring nu : `engineq0`, `COPY_H2D`/`COPY_D2H`, GEMM fp32, oracle | résidence réelle |
| 9 | UAPI + char device + `mmap` + `libvelocitor` (Host/DeviceBuffer, Stream) | chaîne complète |
| 10 | Deux moteurs, deux streams, placement sur nœud | mesures de déséquilibre |
| 11 | Features une par une : bf16, transposition, puis `INDIRECT_DESC`, `EVENT_IDX` | négociation exercée |
| 12 | debugfs complet, `mismatch`, `VEL_IOC_PEEK`, `velocitor-top` | observabilité |
| 13 | Injection d'erreurs, `dma_bits_override`, génération, reprise jusqu'à l'application | ce qui distingue le projet |

Les étapes 0 à 5 constituent le socle PCIe. Les étapes 6 à 9 produisent la chaîne complète
annoncée au §0.1.

**Le point d'arrivée est l'étape 13, pas l'étape 9.** Le §12 est la définition du livrable,
et quatre de ses huit items — 4, 5, 6, 7 — dépendent des étapes 10 à 13. Une chaîne qui
tourne sans mesures ne démontre pas ce que le §0.3 dit vouloir démontrer. Les étapes 10 et 12
sont donc obligatoires, et 11 est la seule vraiment optionnelle.

### 13.1 Couches de test

Trois couches, qui suivent le découpage des étapes. Ce ne sont pas trois produits.

| Couche | Portée | Exemples |
|---|---|---|
| 1 — modèle seul | le device QEMU sans Linux | registres, `SCRATCH`, bornes de fenêtre, latch d'erreur, déterminisme des injections |
| 2 — noyau + modèle | driver dans la VM | probe, MSI-X, `DBG_DMA_*`, remoteproc, table fantôme, négociation, `dma_bits_override=64` |
| 3 — bout en bout | application réelle | oracle numérique et sa tolérance, chaînage sur stream, reprise après crash, comparaisons du §12 |

La couche 3 ne peut exister qu'une fois les injections déterministes (§9) et les versions
épinglées (§C.4) : les trois vont ensemble.

Le modèle QEMU se prête bien à ASan et UBSan, et au *fuzzing* de modèles de périphérique.
**Hors MVP** — bonus naturel une fois le reste vert, jamais avant.

---

## 14. Points ouverts

- **Configuration QEMU pour produire des IOVA hauts** (§9.1), sans quoi le piège DMA n'est
  pas déclenchable même avec `dma_bits_override`. Candidat sur la version épinglée :
  `-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on,aw-bits=48` côté hôte,
  `intel_iommu=on` côté invité — la propriété `aw-bits` existe bien dans QEMU 7.2.22, avec 39
  pour défaut. Ce qui reste ouvert est **empirique** : que les IOVA effectivement distribués
  sous `dma_bits_override=64` dépassent 42 bits. **L'étape 5 ne l'a pas établi.** Le piège
  lui-même est vérifié — la couche 1 du §13.1 le déclenche en écrivant l'IOVA à la main, et
  le modèle rend bien `ERR_CODE = 4` — mais rien ne prouve encore qu'un vrai driver puisse
  l'atteindre : `dma_bits_override` n'existe pas côté driver, et les adresses que produit
  aujourd'hui le bloc `DBG_DMA_*` viennent toutes du pool cohérent, donc sont basses.
  Reporté à l'étape 13, avec le paramètre de module du §9.1.
- **Scatter-gather utilisateur réel** — la v1 s'en tient au tampon cohérent
  (`dma_alloc_coherent`) : une adresse DMA stable, pas de *pinning*. `VEL_F_SG` n'est donc
  pas réellement démontré tant que le chemin `pin_user_pages` + scatterlist + DMA *streaming*
  n'existe pas. À faire en v1.1, après la chaîne complète.
- **Allocateur device** — *bump allocator* par nœud pour commencer, `FREE` invalidant sans
  récupérer. Vrai allocateur seulement quand le besoin apparaît.
- **`VIRTIO_ID_VELOCITOR`** — valeur provisoire, à confirmer contre la spec Virtio de la
  version épinglée. Matérialisée à l'étape 6 sous le nom `VEL_VIRTIO_ID` dans l'en-tête
  partagé, et écrite telle quelle dans la table de ressources : la changer se fera d'un seul
  endroit, mais ne dispense pas de la vérifier.
- **SR-IOV** — extension possible, hors périmètre : répond à une question d'isolation entre
  locataires, pas de parallélisme.
- **Désarmement des injections à usage unique** — les bits 0, 5 et 8 du §9 portent sur « la
  prochaine » opération. Le bit se désarme-t-il quand l'injection est consommée, ou reste-t-il
  jusqu'à ce que le driver l'efface ? L'étape 4 a tranché **pour le bit 6**, l'étape 7 **pour
  le bit 5** : tous deux sont consommés quand ils agissent, faute de quoi l'injection ne serait
  pas reproductible (§16). Les bits 0 et 8 restent ouverts, et le modèle se contente de
  mémoriser ce qui a été écrit — `ERR_INJECT` est donc relisible mais ambigu sur eux : un `cat`
  montre ce qui a été armé, pas ce qui reste armé. Sans conséquence tant que ces injections n'existent pas. **À trancher à
  l'étape 13**, sur le précédent du bit 6 ; le §12 item 6 exige de rendre compte du
  comportement sous chacune, ce qui suppose de savoir laquelle est encore active.

- **Troisième champ du tracepoint `velocitor_irq`** — le driver de l'étape 6 le nomme
  `velocitor_irq_vring` et lui ajoute le retour de `rproc_vq_interrupt()`. **C'est une
  divergence, pas une correction** : le §11 tel qu'il est écrit est réalisable et conforme à
  ce que fait un vrai driver, donc rien n'obligeait à le changer. L'argument pour : le
  `notifyid` est constant par vecteur, donc le seul champ qui varierait est ce retour, et il
  sépare deux pannes qu'on ne distingue pas autrement — une interruption arrivée avant que la
  virtqueue n'existe (§5), et une correspondance vecteur → `notifyid` fausse. L'argument
  contre : `rproc_vq_interrupt()` journalise déjà le `notifyid` par `dev_dbg`, et un champ de
  plus dans un tracepoint du chemin d'interruption se paie à chaque événement. **À trancher
  avant l'étape 7** : soit le §11 gagne le champ et le nom, soit le driver revient à deux.

- **Qualification d'un plantage injecté** — le bit 2 d'`ERR_INJECT` (§9) fait passer
  `FW_STATUS` à 3 et lève le vecteur 5, mais le §4.4 n'a aucun `ERR_CODE` pour « le test a
  demandé un plantage ». Le modèle lève donc le vecteur **sans qualifier**, et le handler
  d'erreur du driver trace un `ERR_CODE = 0` qui se lit « aucune erreur » au milieu d'un
  crash. Inventer un code serait modifier le contrat, pas le modèle. À trancher à l'étape 13,
  avec le reste des injections : soit un code d'injection au §4.4, soit `ERR_INFO` porte le
  masque `ERR_INJECT` responsable et le §4.4 le dit.

Sont sortis des points ouverts en v0.6.3 : les identifiants PCI (§2.1, valeurs assignées) et
le dimensionnement de `VEL_MEM_SIZE`, qui n'était une dette que par rapport à l'étape
safetensors, elle-même retirée du périmètre (§16).

Est sorti à l'étape 0 : le référentiel de versions, désormais épinglé au §C.4.

---

## 15. Annexe A — Conventions et pièges d'implémentation

### A.1 Endianness et alignement

Les structures partagées **avec le device** — messages rpmsg, en-têtes de commande et de
réponse, en-tête firmware, anneau de trace, table de ressources — sont en **little-endian**,
et **toutes** typées `__leXX`.

Ce n'est pas une précaution symbolique : le typage `__leXX` ne matérialise pas le contrat à
lui seul — il faut des conversions explicites aux frontières — mais il permet à *sparse* de
les vérifier. Un typage `__leXX` sur une partie seulement des structures donne le pire des
deux mondes : on croit que le typage protège là où il est présent.

L'**UAPI est hors de ce périmètre** : espace utilisateur et noyau sont sur la même machine,
les types de largeur fixe suffisent.

Dans tous les cas : pas de champ de bits, pas de `int` nu, pas de `enum` dans une structure
partagée. Alignement sur 8 octets, taille multiple de 8.

### A.2 Ordre des accès — hors virtio

**Cette section ne concerne pas les virtqueues.** Côté driver, `virtqueue_add_sgs()` et
`virtqueue_kick()` encapsulent le protocole du ring, barrières comprises ; on ne les
réimplémente pas.

Elle s'applique au **modèle QEMU** et aux structures partagées hors virtio — anneau de trace,
table fantôme, `DBG_DMA_*` :

1. écrire les données ;
2. barrière ;
3. publier l'index ou le pointeur ;
4. barrière ;
5. écrire le registre de notification.

### A.3 Registres à effet de bord

`IRQ_ACK`, `DOORBELL`, `CNT_RESET`, `CNT_SNAP` et `DBG_DMA_CTL` sont en écriture seule et ont
un effet de bord. Le driver ne doit jamais les lire ; le modèle retourne `0xFFFFFFFF` si on
le fait.

Réciproquement, **aucun registre en lecture n'a d'effet de bord** — une relecture spéculative
ne casse rien. C'est pourquoi BAR0 est non-prefetchable, et c'est aussi pourquoi les
compteurs 64 bits sont figés par une écriture de `CNT_SNAP` et non par un verrou déclenché à
la lecture du mot bas : ce dernier mécanisme aurait été un effet de bord en lecture, et une
course entre deux lecteurs.

### A.4 Compteurs

32 bits, débordement silencieux : raisonner en différences, échantillonner assez souvent.
Écrire `CNT_SNAP` avant toute série de lectures devant être mutuellement cohérente.

### A.5 Cycles simulés

`CNT_CYCLES_E*`, `CNT_STALL_E*` et le champ `cycles` des réponses sont des **cycles
simulés**, issus d'un coût forfaitaire par opération majoré de `VEL_FAR_PENALTY` pour les
accès distants.

Grandeurs *relatives*, utiles pour comparer deux placements ou deux politiques dans le même
modèle. Elles ne prédisent rien sur du vrai matériel.

Corollaire : toute mesure du §12 doit être soit un comptage, soit une comparaison entre deux
configurations. Un chiffre absolu ne mesurerait que les constantes du modèle.

### A.6 Ce que le modèle QEMU ne reproduit pas

À écrire noir sur blanc dans le rapport final :

- **le timing réel** : pas de latence PCIe, pas de contention de lien, pas de coût de
  traversée d'IOMMU ;
- **l'ordre effectif des écritures** sur le bus ;
- **le comportement des caches** et la visibilité mémoire réelle ;
- **les conditions de course** que seul le parallélisme physique fait apparaître.

Une CI verte en émulation prouve que la logique est correcte. Elle ne prouve pas qu'un
driver fonctionne sur silicium. C'est utile, insuffisant, et c'est précisément la leçon que
le projet doit permettre de formuler.

---

## 16. Annexe B — Journal des décisions

| Version | Changement | Motif |
|---|---|---|
| v0.1 | Device GEMM sur PCIe, BAR de contrôle, DMA par registres | premier jet |
| v0.1 | API à handles plutôt que calcul sans état | garde ouverte la voie vers une couche de transformeur |
| v0.1 | Accumulation fp32, ordre de réduction non spécifié | force la tolérance numérique et la distinction variation/corruption |
| v0.2 | Renommage → Velocitor ; masque DMA 40 → 42 bits | nom définitif ; valeur non devinable |
| v0.2 | Constantes regroupées | en-tête partagé |
| v0.2 | BAR2 découpé : aperture fixe + fenêtre glissante | la fenêtre seule écraserait les vrings *(motif caduc depuis la v0.6.1)* |
| v0.3 | Plan de données passé de rpmsg à un vdev virtio | rpmsg n'est pas fait pour du volume |
| v0.3 | `UPLOAD`/`DOWNLOAD` supprimés | remplacés par le scatter-gather des descripteurs |
| v0.3 | Deux moteurs, deux nœuds mémoire | ordonnancement et placement mesurables |
| v0.3 | SR-IOV écarté | isolation ≠ parallélisme |
| v0.3 | Compteurs, erreur qualifiée, `ERR_DROPPED`, debugfs, tracepoints | observabilité spécifiée, pas ajoutée |
| v0.4 | Contexte, rationale, annexes | reprise par un intervenant extérieur |
| v0.5 | Ajout du runtime utilisateur : UAPI, char device, `mmap`, `libvelocitor`, `velocitor-top` | la moitié de la compétence visée manquait |
| v0.5 | 4 vrings → 2 : une queue par moteur | `remoteproc_virtio` plafonne à 2 vrings par vdev |
| v0.5 | `VIRTIO_F_VERSION_1` et packed rings retirés | `BUG_ON` sur les features > 32 bits ; `VIRTIO_F_RING_PACKED` effacé par `rproc_transport_features` |
| v0.5 | Séquence remoteproc remise à l'endroit ; `RSC_OFFSET` supprimé | Linux parse l'ELF et résout les ressources avant `start` |
| v0.5 | Carveouts pré-enregistrés, `da_to_va` spécifié | le core alloue par défaut en mémoire hôte |
| v0.5 | Piège DMA inversé : rejet d'un IOVA trop haut | oublier `dma_set_mask` ne produit aucune erreur détectable par le device |
| v0.5 | `FREE` invalide toujours le handle | sinon `ERR_CODE = 3` est indétectable |
| v0.5 | Méchancetés réétiquetées comme artefacts de modèle | la relecture force l'ordonnancement, elle n'est pas la sémantique d'une écriture postée |
| v0.5 | A.2 restreinte au hors-virtio | contredisait la règle « pas d'anneau à la main » |
| v0.6 | `HostBuffer` / `DeviceBuffer` séparés ; `COPY_H2D` / `COPY_D2H` sur le vdev de données | défaut n°1 de la v0.5 : rien ne reliait les deux mémoires |
| v0.6 | Compteur de génération, `{generation, handle}`, `-ESTALE` | après un crash l'allocateur repart de zéro — ABA silencieux |
| v0.6 | Annonce *name service* à la charge du modèle QEMU | sans elle, aucun canal rpmsg n'apparaît |
| v0.6 | `RSC_CARVEOUT` nommé `"heap"`, `da` et `len` explicites | sans concordance de nom, le core retombe sur son allocateur DMA |
| v0.6 | Composition du driver explicitée : trois drivers, trois bus | probes et durées de vie séparés |
| v0.6 | Règles de concurrence tranchées ; UAPI hors périmètre endianness | userspace et noyau partagent la machine |
| v0.6 | `VEL_F_SG` déclassé en v1.1 | tampon cohérent et SG *streaming* sont deux modèles DMA différents |
| v0.6.1 | Vrings en mémoire hôte cohérente, plus dans BAR2 | `remoteproc_virtio` fait `memset()` sur le `va` du carveout sans distinction RAM / I/O memory |
| v0.6.1 | Fenêtre `VQ_*` façon virtio-pci | le device ne connaît ni les adresses ni les `notifyid` : c'est le driver qui les programme |
| v0.6.1 | `RSC_TRACE` abandonné ; trace exposée par le debugfs de Velocitor | `rproc_trace_read()` attend du texte borné par NUL |
| v0.6.1 | Données hors de la chaîne de descripteurs en v1 | le tampon cohérent a déjà une adresse DMA stable |
| v0.6.1 | Une seule commande en vol par queue, dans l'ordre | le FIFO seul ne suffit pas à garantir COPY → GEMM → COPY |
| v0.6.2 | Le contrat de handoff est `VQ_ENABLE`, pas `FW_STATUS = 2` | les virtqueues n'existent pas encore quand le firmware passe à « en cours » |
| v0.6.2 | `find_loaded_rsc_table = NULL` | `rproc_start()` recopierait la `cached_table` par `memcpy()` vers du `__iomem` *(motif correct, conclusion trop large — cf. v0.6.3)* |
| v0.6.2 | Programmer les `VQ_*` depuis `rproc_mem_entry::dma`, jamais depuis `fw_rsc_vdev_vring::da` | `da` est un `u32` ; au-delà de 4 Gio le device lirait à une adresse tronquée |
| v0.6.2 | `vel_host_range` en ligne dans les commandes ; restes SG supprimés du §8 | édition incomplète de la v0.6.1 |
| v0.6.2 | `VEL_F_STEAL` reporté hors v1 | la règle d'ordre vide le vol de son sens |
| **v0.6.3** | **Table de ressources fantôme en mémoire hôte cohérente ; `find_loaded_rsc_table` custom ; `RSC_ADDR_LO/HI/LEN`** | **défaut n°1 de la v0.6.2 : toutes les `config_ops` de `remoteproc_virtio` passent par `rproc->table_ptr`. Sans table visible, le modèle ignore `gfeatures` — donc si `EVENT_IDX` et `INDIRECT_DESC` sont actifs, donc comment parser l'anneau. L'objection de la v0.6.2 visait `__iomem`, pas la table elle-même ; en RAM cohérente elle ne s'applique pas** |
| **v0.6.3** | **`auto_boot = false` et `rproc_add` explicites dans la séquence** | **`rproc_alloc()` pose `auto_boot = true` et `rproc_add()` déclenche alors un chargement asynchrone : la séquence v0.6.2 était incomplète et le devenait dangereuse une fois complétée naïvement** |
| **v0.6.3** | **Vrings préalloués et pré-enregistrés par le driver sous `vdev%dvring%d` ; `VEL_VRING_ALIGN`** | **`rproc_find_carveout_by_name()` n'est pas exportée ; `rproc_alloc_vring()` cherche en revanche un carveout pré-enregistré par ce nom. Le driver détient donc les `dma_addr_t` sans toucher aux structures internes** |
| **v0.6.3** | **Tout le firmware sous 16 Mo ; `da_to_va` avec `is_iomem = true` ; `load = rproc_elf_load_segments`** | **le `heap` commençait exactement où l'aperture finit, donc `da_to_va` rendait `NULL` sur tout ce qu'il était censé servir. Et le chemin fenêtré était déjà exercé par l'étape 4 : la justification du chargeur maison était fausse** |
| **v0.6.3** | **Bloc `DBG_DMA_*` dans BAR0** | **l'étape 5 promettait un DMA bus-master hors virtio sans qu'aucun registre ne permette de le déclencher** |
| **v0.6.3** | **`Stream` dans l'UAPI et le runtime ; deux streams en v1** | **la sémantique de flux du §8.4 n'était pas exprimable : `copy()` et `submit()` ne portaient pas de queue, donc le couple (nœud, moteur) — le sujet mesurable — n'était pas décidable depuis l'application** |
| **v0.6.3** | **`VEL_HOST_POOL_SIZE` et contrainte CMA** | **`dma_alloc_coherent` échoue au-delà de `MAX_PAGE_ORDER` ; un GEMM 1024² fp32 demande 4 Mio par matrice. Sans CMA, la première mesure sérieuse échoue à l'allocation** |
| **v0.6.3** | **« une commande en vol » → « une commande en exécution » par queue** | **la formulation interdisait littéralement plusieurs requêtes postées, ce que contredisaient les 256 descripteurs, le `-EAGAIN` et le §10.3** |
| **v0.6.3** | **Refcount d'usage sur les tampons ; `FREE_*` réussit toujours ; jetons `{generation, seq}`** | **un destructeur RAII ne peut pas rattraper un `-EBUSY` ; le §10.3 promettait un RAII que le §10.2 rendait intenable** |
| **v0.6.3** | **`ERR_CONTEXT` éclaté ; bitmap `IRQ_STATUS` restreint aux vecteurs 0 et 5 ; erreurs synchrones vs fatales** | **`notifyid << 16 \| handle` tronquait un handle 32 bits ; et les vecteurs de queue ne doivent rien acquitter, sans quoi la promesse « aucun MMIO dans le chemin d'IRQ » tombe** |
| **v0.6.3** | **`CNT_SNAP` remplace le verrou de lecture des paires 64 bits** | **la lecture du mot bas figeant le mot haut était un effet de bord en lecture, contredisant A.3, et une course entre `VEL_IOC_STATS` et debugfs** |
| **v0.6.3** | **`CNT_NOTIFY_TX` compte les notifications décidées ; `CNT_NOTIFY_COALESCED` et `CNT_NOTIFY_DROPPED` ajoutés ; granularité de `CNT_DMA_*` définie** | **sans cela l'item 7 du §12 ne révèle rien et l'item 2 n'a pas de dénominateur** |
| **v0.6.3** | **`VEL_F_NODE_HINT` déplacé du vdev de données vers `ctrl_caps` ; `CAPS`/`INFO` croisés dans `mismatch`** | **une capacité gouvernant `ALLOC`, opération rpmsg, était négociée sur le plan de données. Le doublon `CAPS`/`INFO`, lui, devient un diagnostic au lieu d'une ambiguïté** |
| **v0.6.3** | **INT8 retiré du périmètre** | **impose de trancher accumulation, saturation, échelles et zéro-points : de la sémantique numérique sans contrepartie côté driver. bf16 et transposition suffisent à exercer la négociation** |
| **v0.6.3** | **Aucune feature optionnelle négociée à l'étape 8 ; toutes à l'étape 11** | **`INDIRECT_DESC` et `EVENT_IDX` changent le parseur d'anneau : les activer pendant le bring-up mélange deux sources de panne** |
| **v0.6.3** | **En-tête firmware avec magic vérifié ; `FW_STATUS = 1` attribué** | **le chargement était cérémoniel — l'étape 6 réussissait avec un `load` faux — et personne ne posait `FW_STATUS = 1`** |
| **v0.6.3** | **Propriété de `tail` de l'anneau de trace ; instantané debugfs** | **deux `cat` concurrents se volaient les entrées et `dropped` mentait sur la cause** |
| **v0.6.3** | **Injections déterministes, `ERR_INJECT_ARG`, timer virtuel** | **l'item 6 du §12 demande le comportement sous *chaque* injection : un tirage non graine produit une anecdote, pas une mesure** |
| **v0.6.3** | **Handles à partir de 1 ; aliasing A/B/C interdit ; aucune adresse DMA venant de l'espace utilisateur** | **`handle == 0` est le sentinelle « non résident » : un *bump allocator* naïf ferait traiter la première allocation comme une adresse hôte** |
| **v0.6.3** | **Char device enregistré au probe PCI, état `DOWN/BOOTING/READY/CRASHED`** | **contradiction entre §5 et §10.1 ; et l'application doit conserver son descripteur à travers la reprise pour que l'item 6 du §12 soit observable** |
| **v0.6.3** | **Annexe D : obligations du modèle QEMU** | **le modèle est écrit par l'autre intervenant (§0.6) ; son asynchronisme, son comportement au reset et son usage de l'espace DMA PCI étaient des hypothèses tacites** |
| **v0.6.3** | **Point d'arrivée porté à l'étape 13 ; couches de test ; versions épinglées** | **quatre des huit items du §12 dépendent des étapes 10 à 13, alors que C.5 arrêtait le projet à l'étape 9 ; et l'annexe C.4 n'avait pas de référentiel** |
| **v0.6.3** | **Safetensors retiré** | **n'ajoute rien à la preuve visée ; son retrait libère aussi le dimensionnement de `VEL_MEM_SIZE`, qui cesse d'être une dette** |
| étape 0 | Référentiel épinglé : Linux **6.18.44** (longterm), QEMU **7.2.22** (`stable-7.2`) | 7.2.22 est la version de Debian 12, donc celle de la machine de développement : aucun QEMU à maintenir en parallèle du paquet, et un arbre dont la version est déjà connue de l'hôte. Le décalage de trois ans avec 6.18.44 est sans effet — le modèle n'expose que du PCI |
| étape 0 | `aw-bits` confirmé disponible dans l'`intel-iommu` de QEMU 7.2.22 (défaut 39) | le point ouvert du §9.1 passe d'« à trancher » à « candidat identifié, validation empirique à l'étape 5 » (§14) |
| étape 2 | Bloc de compteurs indexé, pas énuméré : `VEL_CNT_FIRST`, `VEL_CNT_LAST`, `VEL_CNT_COUNT`, `VEL_CNT_INDEX()` dans l'en-tête partagé | les 20 compteurs du §4.5 sont contigus et tous 32 bits. Énumérer les offsets des deux côtés, c'est deux occasions de diverger sur *combien* il y en a — et l'écart ne se verrait qu'au moment où un compteur cesserait d'être à zéro |
| étape 2 | Une lecture de compteur avant tout `CNT_SNAP` rend `0` | le §4.5 dit ce que `CNT_SNAP` fige, pas ce que rend une lecture qui ne l'a jamais suivi. L'instantané est mis à zéro au reset, donc la réponse est zéro. L'alternative — rendre la valeur vive tant qu'aucun instantané n'a été pris — donnerait un registre dont la sémantique change après la première écriture de `CNT_SNAP`, ce qui est pire à documenter qu'à coder |
| étape 2 | `CNT_SNAP` et `CNT_RESET` agissent sur le bit 0, pas sur la valeur exacte `1` | le §4.5 dit « écrire 1 ». Exiger l'égalité stricte ferait échouer en silence un driver qui écrit un mot de drapeaux plus large ; agir sur le bit 0 accepte les deux et ne rend aucune écriture ambiguë |
| étape 3 | Table MSI-X à `BAR4 + 0x0000`, PBA à `BAR4 + 0x1000` (`VEL_MSIX_TABLE_OFF`, `VEL_MSIX_PBA_OFF`) | le §3 fixe la BAR et ses 8 Kio, pas le découpage interne. Table à la base et PBA à la moitié suit la convention QEMU et laisse les deux alignés sur 4 Kio. Un driver n'en a jamais besoin — le cœur PCI les lit dans la capacité — mais la couche 1 du §13.1 et une seconde implémentation, si |
| étape 3 | `msix_init()` avec régions explicites, pas `msix_init_exclusive_bar()` | cette dernière code en dur `bar_size = 4096` et produirait une BAR4 de 4 Kio, alors que la taille est contractuelle au §3 |
| étape 3 | `IRQ_MASK` supprime le message MSI-X mais **pas** le latch ; démasquer ne rejoue pas | le §4.1 donne le sens du bit (« 1 = masqué ») sans dire ce qu'il advient de `IRQ_STATUS`. Latcher quand même préserve le diagnostic — le driver voit ce qui s'est passé même s'il n'a pas été réveillé. Ne pas rejouer évite une file d'interruptions différées dont le §3.3 n'a aucun besoin : les deux seuls vecteurs concernés sont la configuration et l'erreur |
| étape 3 | `ERR_INJECT` bit 2 câblé à l'étape 3, avant le bloc d'erreur qualifiée du §4.4 | le critère de l'étape 3 est « interruption **reçue et acquittée** ». Sans un moyen de lever un vecteur, aucun bit ne peut jamais entrer dans `IRQ_STATUS`, donc `IRQ_ACK` reste du code non exercé et l'étape n'est pas falsifiable. Le bit 2 du §9 est le déclencheur le moins cher que le contrat définisse déjà, et il resservira à l'item 6 du §12. Conséquence assumée : le vecteur 5 est levé **sans être qualifié** — pas d'`ERR_CODE = 10` tant que le §4.4 n'existe pas |
| étape 4 | BAR2 en alias de `MemoryRegion`, pas en callbacks d'I/O | des callbacks feraient une sortie de VM par accès de quatre octets, et le critère de l'étape 4 est un balayage des 256 Mo : 64 millions de sorties, ce qui n'est pas un test qu'on lance deux fois. En alias, les deux moitiés tournent à la vitesse de la RAM et le seul MMIO restant sur le chemin est `WIN_BASE` lui-même — ce qui est précisément le registre qu'on veut observer |
| étape 4 | Mémoire locale remplie au reset : chaque mot de 32 bits contient son propre offset | artefact de modèle assumé — le §2 ne dit rien du contenu à l'allumage. C'est ce qui rend le balayage falsifiable : le driver ne peut pas fabriquer cet oracle, puisque écrire un motif à travers la même arithmétique fausse qu'il relira ensuite annulerait les deux erreurs. Un vrai device n'offre **rien de tel** : la mémoire y est indéterminée, et le *bring-up* réel se contente d'un aller-retour. À ne pas prendre pour une pratique transposable |
| étape 4 | `CNT_WIN_MOVE` compte les déplacements effectifs, pas les écritures de `WIN_BASE` | compter les écritures mettrait le compteur d'accord avec un driver qui ne relit jamais — or tout l'intérêt du §9 est que les deux doivent diverger |
| étape 4 | Une écriture de `WIN_BASE` non alignée ou hors plage est ignorée et comptée dans `CNT_ERR_RANGE` | le §3.1 donne les bornes sans dire ce qu'il advient d'une écriture qui les viole. L'ignorer en silence serait indétectable ; lever une erreur qualifiée pour une écriture de configuration serait disproportionné. Le compteur laisse une trace sans rien interrompre |
| étape 4 | Le bit 6 d'`ERR_INJECT` est consommé lorsqu'il agit | le §9 dit « la prochaine écriture ». Le désarmer est la seule lecture qui rende l'injection reproductible : un driver qui compare sa relecture se rétablit à l'essai suivant, un qui ne compare pas reste faux pour de bon. Tranche le point ouvert du §14 pour le bit 6 seulement — les bits 0, 5 et 8 restent à décider avec les injections correspondantes |
| étape 5 | DMA de *bring-up* exécuté depuis un timer sur `QEMU_CLOCK_VIRTUAL`, pas depuis un *bottom half*, et `DBG_DMA_STATUS` passe par `BUSY` | l'annexe D.1 interdit de calculer dans le callback MMIO sans dire par quoi le remplacer. Un *bottom half* ne tourne pas sous `-accel qtest`, où il n'y a pas de CPU : la couche 1 du §13.1 ne verrait jamais un transfert se terminer. Le timer virtuel est déterministe — ce que le §9 exige partout ailleurs — et pilotable par `clock_step`. Copier en ligne rendrait par ailleurs l'état occupé inobservable, et l'étape 5 réussirait avec un driver qui ne scrute rien |
| étape 5 | Un échec de `pci_dma_read`/`write` est rapporté comme `ERR_CODE = 4` | le §4.4 n'a pas de code pour « le bus a refusé ». La largeur d'adresse est de très loin la cause la plus probable ici, et c'est celle que le §9.1 cherche à produire. Approximation assumée, à revoir si un autre mode d'échec apparaît |
| étape 6 | Firmware généré octet par octet par `firmware/mkfw.c`, `EM_NONE`, sans code exécutable | le §6.6 exige un ELF que le core parse réellement, pas un exécutable : le modèle joue le rôle du processeur, et l'image ne prétend donc pas viser une architecture. Un *toolchain* croisé pour un processeur qui n'existe pas serait beaucoup de machinerie pour moins de contrôle ; le générateur partage `velocitor_hw.h` avec le modèle et le driver, donc ne peut pas diverger d'eux |
| étape 6 | En-tête firmware et en-tête d'anneau de trace exprimés en offsets dans l'en-tête partagé, pas en `struct` | `velocitor_hw.h` ne dépend d'aucun autre en-tête et n'a donc aucun type de largeur fixe pour déclarer des champs. Les trois consommateurs ont les leurs ; le §6.6 garde la forme `struct` comme documentation |
| étape 6 | La position de l'anneau de trace est choisie par le générateur (64 Kio) et publiée par l'en-tête, pas figée par une constante partagée | c'est exactement ce à quoi sert le champ `trace_da` du §6.6 ; une constante partagée le viderait de son sens. Le modèle vérifie en revanche que l'anneau tient entièrement dans l'aperture fixe, faute de quoi le driver ne pourrait pas le lire (§6.2) |
| étape 6 | Le `PT_LOAD` déclare un `p_memsz` qui couvre l'anneau de trace | `rproc_elf_load_segments` fait alors le `memset_io()` de la zone, donc `head`, `tail` et `dropped` valent zéro sans que le firmware ait à livrer un anneau prérempli — ce qui serait un mensonge sur ce qu'il a écrit. Seul `entry_size` est posé, par le modèle, au démarrage |
| étape 6 | À la montée de `RSC_VALID`, le modèle ne vérifie que `ver` et `num` | parser les entrées appartient à l'étape qui les consomme, et redoubler la validation du core ne créerait qu'un second avis susceptible de le contredire. Ces deux mots suffisent à attraper un driver qui publie le mauvais tampon, ou le bon à la mauvaise adresse |
| étape 6 | `RSC_VALID` relit ce que le device a **accepté**, pas ce qui a été écrit | sans quoi un driver dont la table est illisible croirait l'avoir publiée, et ne le découvrirait qu'à l'étape 7, sur un `notifyid` aberrant |
| étape 6 | Le piège des 42 bits s'applique aussi au chemin de contrôle | il serait absurde qu'une table publiée hors de portée du device soit acceptée alors qu'un transfert de données à la même adresse est refusé : c'est le même défaut de masque, au même endroit |
| étape 6 | `RESET` relâchée sans table fantôme valide : journalisée, pas refusée | le §6.1 fixe l'ordre — publier, puis relâcher — mais le démarrage lui-même n'a pas besoin de la table. La refuser inventerait une dépendance que le contrat ne crée pas ; la taire laisserait passer un bug qui n'éclaterait qu'à l'étape 7 |
| étape 6 | `RESET` vaut 1 au démarrage ; `GENERATION` part de 0 et la première réussite la porte à 1 | rien n'est chargé, donc annoncer autre chose serait un mensonge sur lequel le driver pourrait agir. Et « aucun firmware n'a jamais tourné ici » se distingue ainsi de « un seul l'a fait » (§6.5) |
| étape 6 | Relâcher `RESET` repart toujours de `FW_STATUS = 0`, y compris en sortie de `CRASHED` | le §4.1 dit qu'un en-tête invalide laisse `FW_STATUS` à 0 ; cela doit valoir en sortie de plantage comme à froid, sinon la reprise du §6.5 hérite du statut de la génération précédente |
| étape 6 | Le passage `FW_STATUS` `1 → 2` est différé par un timer virtuel, comme le DMA | sans délai, l'attente de `FW_STATUS == 2` dans `ops->start()` serait une formalité qui n'attend rien, et un driver qui ne scruterait pas passerait tout de même |
| **étape 6** | **Fenêtre `VQ_*` programmée dans `ops->start()`, plus au retour de `rproc_boot()` (§5, §6.5)** | **`rproc_boot_recovery()` enchaîne `rproc_stop()` + `rproc_start()` sans repasser par `rproc_boot()` ni par `prepare` : tout ce qui était programmé après le retour n'avait lieu qu'une fois, alors que le §6.5 remet tous les `VQ_ENABLE` à zéro au plantage. La spec décrivait une reprise qu'aucun des deux côtés n'aurait pu mener à bien — elle invalidait le transport sans dire qui le rétablissait** |
| **étape 6** | **Le driver remet les quatre anneaux à zéro à chaque démarrage (§4.2, §5)** | **`vring_new_virtqueue()` remet ses compteurs à zéro sans toucher la mémoire de l'anneau, que le driver a allouée. À la génération suivante, `avail->idx` porte encore la valeur de la précédente pendant que le device repart de zéro : il consommerait des descripteurs morts. Même ABA que le §6.5, transposé du handle vers l'anneau — et invisible tant que personne ne provoque un crash** |
| **étape 6** | **`gfeatures` et le statut virtio relus à chaque usage de la queue, pas une fois à `VQ_ENABLE` (D.4)** | **conséquence directe du déplacement ci-dessus : l'activation précède désormais le démarrage des sous-périphériques, donc la négociation. Deux lectures DMA par doorbell contre une valeur qui serait systématiquement fausse** |
| **étape 6** | **Le bloc d'erreur conserve la **première** erreur non acquittée ; les suivantes n'incrémentent que `ERR_DROPPED` (§4.4)** | **le §4.4 définit `ERR_DROPPED` sans dire laquelle des deux survit. Garder la dernière donnerait au driver le symptôme d'une cascade avec la cause écrasée, ce qui vide le registre de son intérêt : « ce que le driver n'a pas vu » doit inclure ce qui l'a déclenché** |
| étape 6 | Le parcours des `notifyid` a lieu à la montée de `RSC_VALID`, la validation reste `ver` et `num` | le device a maintenant besoin de la carte `notifyid` → queue, puisque le `DOORBELL` ne porte rien d'autre (§4.2). Mais une table qu'il ne sait pas parcourir n'est pas pour autant invalide : elle coûte le routage, pas la publication. Le journal le dit à la montée de `RSC_VALID`, pas au premier doorbell |
| étape 6 | Le balayage de l'`avail` compte les têtes et avance son index, sans rien consommer | l'étape 6 ne peut honnêtement prétendre qu'à ceci : le doorbell arrive, les adresses publiées sont lisibles en bus-master, `CNT_DB_RX` et `CNT_DESC` le disent. Consommer appartient au §7 et à D.5. Ne pas avancer l'index recompterait les mêmes têtes à chaque doorbell — un compteur faux est pire ici qu'un compteur incomplet (D.6) |
| **étape 7** | **Le déclencheur de l'annonce NS passe de « les deux vrings activés » au premier doorbell où `F_NS` est négocié (§7.1)** | **conséquence directe du déplacement de la fenêtre `VQ_*` dans `ops->start()` : l'activation précède désormais la négociation, donc la condition que le §7.1 pose lui-même — n'annoncer que si `F_NS` est dans `gfeatures` — ne pouvait plus être satisfaite au moment qu'il prescrivait** |
| **étape 7** | **Le doorbell arme un timer virtuel ; le balayage n'a jamais lieu dans le callback MMIO (D.1)** | **le modèle levait le vecteur de complétion avant que le `writel()` de l'invité n'ait rendu la main, donc l'interruption arrivait sur la pile de l'émetteur — qui détient le `tx_lock` de `virtio_rpmsg_bus`. Une réponse menant à un nouvel envoi s'auto-bloquait dessus. L'annexe D.1 l'interdisait déjà ; c'est la première fois que le modèle en avait l'occasion** |
| **étape 6** | **Valeurs de `level` (0 info, 1 warn, 2 error) et sentinelle `seq = 0` pour une entrée sans opération, dans l'en-tête partagé** | **le §6.6 donne les deux champs sans donner leurs valeurs, et on ne peut pas écrire une entrée sans en choisir. Trois niveaux sont ce que le modèle sait honnêtement distinguer aujourd'hui ; `seq` a besoin d'un « sans objet » comme `engine` a le sien, puisque le journal du firmware précède de loin la première opération portant un jeton (§10.2)** |
| **étape 6** | **`timestamp` porte des nanosecondes d'horloge virtuelle depuis le démarrage du firmware, pas des cycles simulés** | **le §6.6 dit « cycles simulés depuis le reset », mais le §A.5 fait des cycles un coût forfaitaire *par opération* — il n'en existe aucune source tant que les moteurs ne sont pas écrits, et le champ vaudrait 0 sur chaque ligne. L'horloge virtuelle est déterministe (D.6) et pilotable par `clock_step`, donc elle garde la propriété qui compte : deux exécutions identiques produisent les mêmes horodatages. **À revoir à l'étape 10**, quand les moteurs donneront un vrai compteur de cycles** |
| **étape 6** | **`head`, `tail` et `dropped` de l'anneau de trace sont des compteurs libres, la case étant `index % VEL_TRACE_ENTRIES` (§6.6)** | **le §6.6 donnait la structure sans dire ce que valaient les index. Des indices repliés imposent une convention pour séparer « plein » de « vide », donc une occasion de plus pour les deux implémentations de diverger ; des compteurs libres rendent « vide » et « en retard » calculables par une soustraction non signée, et la resynchronisation du driver explicite. Même forme que l'`avail->idx` de virtio, pour la même raison** |
| **étape 7** | **`FREE` reçoit une requête, `vel_free_req { __le32 handle; __le32 reserved; }` (§7.2)** | **le §7.2 nommait l'opération sans lui donner de structure, et une opération qui invalide un handle doit dire lequel. Le driver a découvert le trou en dimensionnant ses requêtes : `FREE` s'y retrouvait à côté d'`INFO` et de `STAT`, sans charge utile. `reserved` porte le handle nulle part : le §7.2 le dit écrit à zéro et ignoré en lecture, donc s'en servir contredirait le contrat au lieu de le compléter** |
| **étape 7** | **Le driver n'accepte du device qu'un `status` nul ou dans `[-MAX_ERRNO, -1]`, et vérifie que la réponse porte l'`op` demandée (§7.2, §9)** | **le §9 fait du mensonge du modèle une fonctionnalité, et l'appariement par `seq` seul ne dit pas que la réponse répond à la question posée. Un `status` non filtré devient un numéro d'erreur inventé au retour d'un ioctl du §10.2 ; une réponse trop courte recopiée telle quelle donnerait au diagnostic croisé du §7.3 un écart qui serait celui du driver, pas celui du firmware — soit précisément le faux positif que le §11 dit vouloir éviter** |
| **étape 7** | **`VEL_NODE_ANY` sert le premier nœud qui peut tenir le bloc, jamais le plus libre** | **le §3.2 fait des deux nœuds des tailles différentes et le §12 demande de *mesurer* le déséquilibre. Une politique qui égaliserait rendrait un chiffre plus joli en masquant exactement ce que la mesure existe pour montrer, ce que le §0.3 interdit. Premier-qui-tient est en plus déterministe, donc conforme à D.6 : deux exécutions identiques placent les mêmes blocs aux mêmes offsets** |
| **étape 7** | **Les bornes allouables de chaque nœud sont énoncées dans l'en-tête partagé (`VEL_NODE0_BASE` … `VEL_NODE1_END`), pas recalculées de chaque côté** | **l'aperture fixe est prélevée sur le nœud 0, donc les deux nœuds n'ont pas la même capacité — une asymétrie que le §3.2 expose délibérément et sur laquelle les deux implémentations doivent tomber d'accord. Une arithmétique refaite deux fois est une occasion de diverger de plus, et c'est précisément ce que le §C.2 veut supprimer** |
| **étape 7** | **La table des handles du modèle est plafonnée à 1024 par session, et ce plafond n'est pas dans l'en-tête partagé** | **le §7.2 n'énonce aucune limite, donc ce n'est pas du contrat mais une borne du modèle. Elle existe pour que le device n'alloue rien à l'exécution : la migration reste un tableau de taille fixe et le comportement reste déterministe (D.6). Très au-delà de ce que l'allocateur *bump* du §14 rend significatif, et à revoir en même temps que lui** |
| **étape 7** | **Un `node` malformé dans `ALLOC` est refusé par `-EINVAL` sans qualifier le bloc d'erreur du §4.4** | **aucun des dix `ERR_CODE` ne désigne un argument invalide, et en inventer un serait modifier le contrat plutôt que le modèle (§0.6). Le refus reste visible en `LOG_GUEST_ERROR` ; le bloc d'erreur garde la dernière erreur *qualifiable*, ce qui vaut mieux qu'un code approximatif que le driver rapporterait comme une panne du device** |
| **étape 7** | **Le bit 5 d'`ERR_INJECT` est consommé quand il agit, comme le bit 6** | **deuxième des trois bits « à usage unique » du §14 à devenir réel, tranché comme le premier et pour la même raison : une injection qui resterait armée frapperait toutes les allocations suivantes, et le §12 item 6 demande le comportement *sous* chaque injection, pas après la première. Reste ouvert pour les bits 0 et 8** |
| étape 6 | Une écriture de description sur une queue activée est refusée et journalisée | le §4.2 met `VQ_ENABLE` en dernier pour que la description soit complète au moment de l'activation. En accepter une ensuite reviendrait à laisser le device lire un anneau dont l'adresse change sous lui ; la refuser transforme un bug de driver en ligne de log |

### Décisions écartées, et pourquoi

| Option | Écartée parce que |
|---|---|
| Firmware réel compilé pour un second cœur | plusieurs semaines, déplace l'apprentissage vers le bare-metal |
| Anneau de descripteurs maison | `virtio_ring` existe côté hôte |
| Hook BPF dédié | demande un support dans le vérificateur ; les tracepoints donnent déjà l'attachement BPF |
| SR-IOV pour le parallélisme | mauvais outil : isolation ≠ parallélisme |
| Mémoire des matrices côté hôte uniquement | supprime la résidence, qui est le sujet intéressant |
| Aperture BAR2 directe sur 256 Mo | n'exerce ni la traduction d'adresse ni le read-back |
| Quatre vrings sur un seul vdev | rejeté par le transport remoteproc (`RVDEV_NUM_VRINGS = 2`) |
| `UPLOAD`/`DOWNLOAD` en rpmsg | correct de les retirer de rpmsg ; leur suppression pure était une erreur — revenus en v0.6 comme `COPY_*` |
| `Buffer` unique couvrant hôte et device | masquait deux mémoires distinctes derrière un seul type |
| Vrings dans la mémoire locale exposée par BAR2 | `remoteproc_virtio` les traite comme de la RAM CPU, pas comme du `__iomem` |
| Table de ressources en BAR2 | même raison ; **mais la supprimer entièrement était une erreur** — elle revient en v0.6.3 en RAM cohérente |
| `rproc_elf_find_loaded_rsc_table` générique | résout la section ELF via `da_to_va` et rendrait un pointeur BAR2 |
| `RSC_TRACE` pour un anneau binaire | le lecteur générique attend du texte borné par NUL |
| Registre unique `IRQ_NOTIFYID` | course entre vecteurs, et une lecture MMIO par interruption |
| Registre `VQ_NOTIFYID` | redondant avec la table fantôme ; deux sources de vérité |
| Verrou de lecture sur les paires de compteurs | effet de bord en lecture, et course entre deux lecteurs — remplacé par `CNT_SNAP` |
| Vol de travail entre queues en v1 | incompatible avec une seule commande en exécution par queue |
| Chargeur ELF maison à fenêtre glissante | inutile une fois le firmware contraint sous 16 Mo ; l'étape 4 exerce déjà la fenêtre |
| INT8 en v1 | sémantique numérique coûteuse, aucun apport côté driver |
| Safetensors | une matrice synthétique suffit à mesurer DMA, placement et queues |
| Carveouts symétriques pour égaliser les nœuds | dépenserait de la mémoire pour rendre un chiffre plus joli ; l'asymétrie est exposée par `STAT` |

---

## 17. Annexe C — Pour reprendre le travail

### C.1 Ordre de lecture

1. §0 — le contexte et les raisons.
2. §1 — la forme générale, et surtout §1.1, la couche que ce document spécifie.
3. §2, §3 — le contrat matériel.
4. §13 — les étapes, pour situer l'avancement.
5. §16 — le journal, pour ne pas reproposer une option déjà écartée.
6. Annexe D si vous écrivez le modèle QEMU.
7. Le reste au fil du besoin.

### C.2 Première chose à produire

**L'en-tête partagé** (§2, §6.6, §7.2, §8.3, §10.2), consommé par le modèle, le driver et la
bibliothèque. Tant qu'il n'existe pas, les trois côtés divergeront sur des détails de
disposition mémoire.

Juste avant : **exécuter la liste de vérification du §C.4**. Le référentiel est épinglé
— Linux 6.18.44, QEMU 7.2.22 — mais sa vérification ne l'est pas, et c'est elle qui dit si
le §6 tient encore.

### C.3 Comment trancher un point ouvert

Les questions non résolues sont listées au §14 et signalées par **à trancher** dans le
texte. Règle : trancher au plus simple, noter la décision au §16 avec son motif, ne
complexifier que quand le besoin apparaît dans le code.

Exception à cette règle, et il faut la connaître : quand une simplification supprime
l'exercice d'une compétence listée au §0.2, le §0.2 l'emporte. C'est ainsi que la table
fantôme a été préférée à la suppression pure et simple de la négociation virtio, alors que la
seconde était plus simple.

### C.4 Vérifier avant d'implémenter

**Référentiel épinglé.** Toute affirmation de ce document s'entend contre ces deux tags, et
contre eux seuls :

| Composant | Version | Où, et pourquoi celle-là |
|---|---|---|
| Linux | **6.18.44** (longterm) | noyau de l'invité, construit dans la VM |
| QEMU | **7.2.22** (branche `stable-7.2`) | hôte Debian 12 : le paquet `1:7.2+dfsg-7+deb12u18` **est** 7.2.22. Mais le modèle se construit *dans l'arbre* QEMU, donc à partir du tag upstream `v7.2.22`, jamais du paquet |

L'écart de trois ans entre les deux est sans conséquence sur le fond : le modèle n'expose que
du PCI, et l'invité n'a aucun besoin d'un hôte contemporain. Il en a une sur la forme du code
du modèle, qui vise l'API QOM de 2022 et non celle des QEMU 9 et 10 dont parlent la plupart
des exemples disponibles.

Ce document s'appuie sur des contraintes du noyau Linux qui peuvent bouger. **Aucun des
points ci-dessous n'a encore été vérifié contre ces tags ; c'est le premier travail de
l'étape 0 :**

- `RVDEV_NUM_VRINGS` dans `include/linux/remoteproc.h` ;
- le `BUG_ON` sur les features > 32 bits dans `rproc_virtio_finalize_features` ;
- l'effacement de `VIRTIO_F_RING_PACKED` dans `rproc_transport_features` ;
- que `rproc_virtio_finalize_features` écrit bien `rsc->gfeatures` dans `rproc->table_ptr`,
  et que le statut virtio y transite aussi ;
- l'ordre `parse_fw` → `handle_resources` → `load` → `find_loaded_rsc_table` → `start` →
  `start_subdevices` dans `rproc_fw_boot` / `rproc_start` ;
- que `rproc_alloc()` pose `auto_boot = true` et que `rproc_add()` déclenche
  `rproc_trigger_auto_boot()` ;
- que `rproc_alloc_vring()` cherche un carveout pré-enregistré nommé `vdev%dvring%d`, et les
  conditions exactes de `rproc_check_carveout_da()` ;
- la liste des symboles exportés utilisés : `rproc_mem_entry_init`, `rproc_add_carveout`,
  `rproc_da_to_va`, `rproc_boot`, `devm_rproc_add`, `rproc_report_crash`,
  `rproc_elf_load_segments`, `rproc_elf_load_rsc_table` — et l'absence d'export de
  `rproc_find_carveout_by_name` ;
- le chemin `is_iomem` de `rproc_elf_load_segments` (`memcpy_toio` / `memset_io`) ;
- le masque DMA par défaut et le comportement de `dma_set_mask_and_coherent` ;
- la limite de `dma_alloc_coherent` sans CMA (`MAX_PAGE_ORDER`) ;
- la création dynamique des canaux rpmsg : `VIRTIO_RPMSG_F_NS` et le format de l'annonce
  *name service* ;
- la largeur de `fw_rsc_vdev_vring::da` et l'affectation de `mem->da` dans
  `rproc_alloc_carveout()` ;
- côté QEMU : les API `pci_dma_read()` / `pci_dma_write()`, les timers
  `QEMU_CLOCK_VIRTUAL`, et les options du vIOMMU pour la largeur d'IOVA.

Une divergence constatée est une information, pas un obstacle : la noter au §16.

### C.5 Critère d'arrêt

Le projet est un moyen. Il a atteint son but quand les étapes 0 à 10, 12 et 13 sont faites
**et** que les huit items du §12 peuvent être énoncés avec des comptages et des comparaisons.
L'étape 11 est le seul vrai bonus.

Ce critère est plus exigeant que celui des versions précédentes, et c'est délibéré : la
version antérieure arrêtait le projet à l'étape 9, alors que la moitié du §12 dépend des
étapes suivantes. Une chaîne qui tourne sans mesures ne démontre pas ce que le §0.3 dit
vouloir démontrer.

Le risque réel n'est pas de mal faire — c'est de ne jamais finir parce qu'on aura ajouté une
fonctionnalité de plus au lieu de mesurer celle d'avant.

### C.6 La spec est close

Quatre tours de revue. Les trois premiers ont produit des corrections réelles ; le quatrième
aussi — il a trouvé que la correction majeure du troisième avait rendu la négociation virtio
invisible au modèle, qu'il manquait `rproc_add()`, et qu'une étape entière n'avait aucun
protocole. Ce n'était pas du raffinement.

C'est le mode de défaillance des éditions par patches successifs : chaque correction est
juste, et personne ne reparcourt la chaîne complète ensuite. La v0.6.3 a été relue **comme une
chaîne**, pas comme une liste de correctifs.

**La conception s'arrête ici. La prochaine action est l'étape 0**, pas une v0.6.4.

Les modifications ultérieures se font au fil de l'implémentation, quand le code révèle une
impossibilité — et se notent au §16 avec leur motif. Ajouter bf16 hors étape 11, SR-IOV,
safetensors ou toute autre fonctionnalité avant que la chaîne des étapes 0 à 13 tourne serait
contre-productif.

---

## 18. Annexe D — Obligations du modèle QEMU

Le modèle est écrit par un autre intervenant (§0.6). Ce qui suit est la partie du contrat qui
ne se déduit pas des sections précédentes.

### D.1 Asynchronisme

Le modèle **ne calcule jamais dans le callback MMIO ou doorbell**. Chaque moteur est une
machine à trois états — *inactif*, *en exécution*, *complétion en attente* — pilotée par des
timers `QEMU_CLOCK_VIRTUAL` et des *bottom halves*. Un GEMM entier calculé dans le callback
bloquerait la boucle d'émulation ; le modèle ne simulerait plus un accélérateur, il gèlerait
la machine.

Le bit 3 d'`ERR_INJECT` utilise un timer virtuel de `ERR_INJECT_ARG` millisecondes, **jamais
un `sleep()`** : la CI ne doit pas payer cinq secondes de temps mur.

`DBG_DMA_*` est asynchrone au même titre : `DBG_DMA_CTL` arme le transfert, `DBG_DMA_STATUS`
passe à 1 puis à 2.

### D.2 Accès mémoire

Tous les accès à la mémoire hôte passent par l'espace d'adressage DMA du périphérique PCI —
`pci_dma_read()` / `pci_dma_write()` — et **jamais** par la mémoire invitée directement. Les
valeurs de `VQ_DESC/AVAIL/USED`, de `RSC_ADDR_*` et de `vel_host_range.dma_addr` sont des
adresses vues par le device, pas des adresses physiques invitées.

C'est la condition pour que le test vIOMMU du §9.1 mesure quoi que ce soit : y accéder
directement court-circuiterait la traduction qu'on cherche précisément à exercer.

Tout IOVA dépassant `VEL_DMA_BITS` est rejeté avec `ERR_CODE = 4` avant tout accès.

### D.3 Reset

`RESET = 1` doit, dans cet ordre :

1. annuler tout travail différé — timers, *bottom halves*, transferts `DBG_DMA` en cours ;
2. interdire tout nouvel accès DMA ;
3. remettre tous les `VQ_ENABLE` à zéro et purger l'état des queues ;
4. invalider `RSC_VALID` ;
5. repasser `FW_STATUS` à 0.

Sans le point 1, un GEMM en vol au moment d'un crash écrirait dans une mémoire réallouée à la
génération suivante — le même ABA que le §6.5 combat côté hôte, laissé ouvert côté device.

Au relâchement de `RESET`, le modèle vérifie `vel_fw_hdr` (§6.6) avant de poser
`FW_STATUS = 1`, puis 2, et d'incrémenter `GENERATION`.

### D.4 Ce que le modèle lit, et quand

| Information | Source | Disponible à partir de |
|---|---|---|
| `notifyid` de chaque vring | table fantôme | `RSC_VALID = 1` |
| `gfeatures` de chaque vdev | table fantôme | premier doorbell sur une queue du vdev |
| statut virtio | table fantôme | idem |
| adresses des anneaux, vecteur MSI-X | registres `VQ_*` | `VQ_ENABLE = 1` |

Le modèle **n'écrit jamais** dans la table fantôme : elle appartient à Linux.

> **`gfeatures` n'est pas disponible à `VQ_ENABLE`.** La v0.6.3 le supposait, parce qu'elle
> plaçait la programmation de la fenêtre au retour de `rproc_boot()` — après la négociation.
> Depuis que le §5 la place dans `ops->start()`, l'activation précède le démarrage des
> sous-périphériques : à ce moment la table ne contient encore que des zéros.
>
> Le modèle relit donc `gfeatures` et le statut **à chaque usage de la queue**, pas une fois
> à l'activation. Deux lectures DMA par doorbell, et la valeur est toujours celle du moment
> où elle sert. La règle qui suit est inchangée, et c'est elle qui compte.

Le modèle parse l'anneau selon `gfeatures`, jamais selon `dfeatures` — confondre les deux
revient à parser selon ce qui a été offert et non selon ce qui a été accepté.

### D.5 Endpoint rpmsg

Le modèle implémente la moitié distante du transport `virtio_rpmsg_bus` : consommation des
*split rings*, gestion des tampons de réception fournis par Linux, construction de
`rpmsg_hdr`, respect des adresses source et destination. Il émet l'annonce *name service*
selon §7.1 — **au premier doorbell où `VIRTIO_RPMSG_F_NS` figure dans les `gfeatures` de la
table fantôme et où un tampon de réception est disponible**, et non à l'activation des
vrings : depuis que le §5 place la programmation de la fenêtre dans `ops->start()`,
`gfeatures` vaut encore zéro à cet instant et la condition s'interdirait elle-même. Le §7.1
détaille le raisonnement.

### D.6 Ce qui doit rester déterministe

Aucun tirage aléatoire nulle part. Coalescence, corruption périodique, délai, suppression de
notification : tous pilotés par des compteurs ou des timers remis à zéro à l'armement (§9).

Les compteurs sont la source de vérité indépendante du driver : ils doivent être exacts même
quand le device se comporte mal, et surtout à ce moment-là.
