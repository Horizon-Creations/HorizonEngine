# hpak — Packaging-Format & Loading-Plan

> Status: **Plan** (2026-07-01). Beschreibt den **Ist-Zustand (v1)** des Game-Packagings
> und den geplanten **Umbau auf hpak v2** (Format, Kompression, Verschlüsselung, Runtime-Loading).
>
> Grundlage: Analyse der bestehenden Implementierung (primär, mit `file:line`-Referenzen) +
> Recherche zu UE `.pak`/IoStore, Unity AssetBundles/Addressables, zstd/LZ4, mmap & Krypto.
>
> **Getroffene Entscheidungen** (Produkt-Weichen, siehe [§0](#0-entscheidungen)):
> 1. Verschlüsselung: **AES-256 (Key im Client gebacken)** — ehrlich als *Obfuskation*, nicht als Sicherheitsgrenze.
> 2. Kompression: **zstd (+ trainiertes Dictionary) fürs Shipping, LZ4 als Schnellpfad**.
> 3. Loading: **Voller Umbau — mmap + on-demand Streaming + Pin/Handle-Integration**.

---

## 0. Entscheidungen

| Weiche | Wahl | Ehrliche Konsequenz |
|---|---|---|
| **Krypto** | AES-256-GCM, Key gebacken | Der generische `HorizonGame`-Runtime + `project.hcfg` werden **zusammen** ausgeliefert → der Key liegt zwangsläufig beim Client. Das ist **Obfuskation gegen casual Ripping**, kein Schutz. UnrealKey zieht AES-256 aus laufenden UE-Spielen in Sekunden — wir machen es nicht besser als Epic. Wert: hebt die Latte von „in einen Ripper ziehen / Hex-Editor" auf „Debugger anhängen + Key aus dem RAM dumpen". |
| **Kompression** | zstd (+Dict) ship, LZ4 schnell | zstd bringt den echten Ratio-Sprung (Level 1 ~2.9× > LZ4HC ~2.7×; -19 ~3.5×; Dictionary bei vielen kleinen Assets bis ~2×–5× obendrauf). Kostet eine neue C-Lib (wie LZ4 aus Source baubar). LZ4 bleibt der schnelle Encode-Pfad fürs Editor-Iterieren. |
| **Loading** | mmap + on-demand + refcounted | Größter Skalierungsgewinn (weg vom eager Whole-Pak-in-RAM). **Caveat:** mmaps Zero-Copy-Vorteil ist gedämpft, weil Entries komprimiert+verschlüsselt sind → der Reader allokiert ohnehin einen Decode-Buffer. Echte Gewinne: kein fd-Reopen pro Read, OS-Page-Cache managt Residency, Zero-Copy nur für `store`+unverschlüsselte Entries. |

**Leitprinzip (Was jetzt / Was später):**

- **Layout-Änderungen sind JETZT gratis** — `k_version == 1` ist unveröffentlicht, es existiert kein
  ausgeliefertes Spiel, das v1-Paks laden müsste. Also **alle Format-Felder in einen Version-Bump packen**
  (auch reservierte Felder für Features, deren Runtime-Code erst später kommt).
- **Verhaltens-/Runtime-Änderungen brechen das Format nicht** und können jederzeit landen → getrost staffeln.

Kein Backward-Compat-Zwang zu v1: der Reader lehnt `version != 2` sauber ab (wie heute schon `version != 1`,
[HpakReader.cpp:20](../src/HE_Core/src/Hpak/HpakReader.cpp)). Ein Re-Export erzeugt v2 aus denselben `.hasset`-Quellen.

### Implementierungsstatus (2026-07-02)

| Milestone | Status |
|---|---|
| **M1 Format v2** (FileHeader 64B, EntryDesc 56B, sortierte TOC + Binärsuche, `tocHash`/`contentHash` FNV-1a, `codec`-Byte, persistenter File-Handle statt fd-Reopen) | ✅ **erledigt** |
| **M2 Kompression** (LZ4HC-9 statt `LZ4_compress_default`, zstd -19 via System-Lib; Dictionary-Training noch offen) | ✅ **erledigt** (Dict-Training offen) |
| **M3 Verschlüsselung** (AES-256-GCM via OpenSSL EVP, per-Entry-Nonce, Auth-Tag; Random-Key in `project.hcfg`; **D1** gefixt: `GameApplication` übergibt Key an `loadPak`) | ✅ **erledigt** |
| **Bonus-Fix** | `loadAssetFromMemory` (Pak-Ladepfad) dedupliziert → delegiert an `parseAndRegisterAsset`. Behebt **verifizierten Bug**: SkeletalMesh/Scene/Script/Font/Shader wurden aus jedem Pak still verworfen. |
| **M4 Runtime-Loading** ✅ **erledigt**: `ContentManager::mountPak` (lazy, ohne Parsen) + UUID→Entry-Residency-Index + **Overlay-Stack** (späterer Mount shadowt früheren per UUID = Patch/DLC/Mod) + **on-demand** synchron via `acquireXxx`/`ensureResident` UND **async** via `loadAssetAsync(UUID)` + `streamMountedAssets()` (Worker liest+dekodiert off-thread, `pollAsyncResults()` registriert am Frame-Ende auf dem Main-Thread — sicher, weil `SlotMap` `std::vector`-backed ist). **GameApplication geflippt**: eager `loadPak` → `mountPak` + `streamMountedAssets()` (nicht-blockierender Start; Assets poppen über die ersten Frames rein, Renderer überspringt nicht-residente = kein Crash) + **budgetiertes `pollAsyncResults(16)` je Frame**. Off-Thread: I/O + Decompress (LZ4/zstd) + Decrypt (AES). Auf dem Main-Thread bleibt nur Parse+Register — **pro Frame gedeckelt** (Default `SIZE_MAX` = alles; das Spiel gibt 16), damit ein Burst gleichzeitig fertiger Loads über Frames verteilt wird statt einen Frame einzufrieren. (Verbleibender Main-Thread-Kostenpunkt außerhalb hpak: der First-Sight-GPU-Upload im Renderer — separates Renderer-Thema.) **Backend: read()/persistenter Handle** statt mmap — bewusst (mmap-Zero-Copy hier gedämpft wegen comp+enc + SIGBUS-Risiko bei truncation vs. saubere Integritäts-Checks). |
| **D8 Binäre Startup-Szene ins Pak** ✅ **erledigt**: Editor serialisiert die *gespeicherte* Szene (frisch von Platte, nicht die Live-Welt) via `SceneSerializer::saveToMemory` (CBOR) → `ProjectExporter` packt sie unter einer generierten `startupSceneUuid` INS Pak (kein loses JSON mehr) → hcfg trägt `hasPackedScene`+`startupSceneUuid`. GameApplication liest sie via neuem `ContentManager::readMountedEntry` (Raw-Entry, nicht als Asset geparst) → `loadFromMemory`, und **schließt die Szene-UUID vom Streaming aus** (`streamMountedAssets(exclude)`). Loses-JSON bleibt Fallback. |
| **M5 Tests** | ✅ alle 10 Asset-Typen × {Store,LZ4,zstd} + verschlüsselt; Integrität (TOC/Content-Korruption), Reader-Poison-Regression, AES wrong-key/Nonce-Uniqueness, hcfg-Key end-to-end, mount on-demand + overlay-shadowing + verschlüsselter Mount, async-Streaming aus Pak, ProjectExporter→mount→stream→poll, **binäre Szene round-trip durch den Pak + Streaming-Exclude**. **503 Tests grün.** |

Verbleibend / bewusst offen: **Referenzgraph / „nur Gebrauchtes laden"** — Design steht (s. §6.5: Ursache = pfad-basierte Intra-Refs; Option (a) Pfad→UUID-Manifest + Frontier-Expansion empfohlen), Implementierung offen. **In-Window-Visual-Verify des Pop-in** (braucht Display + gepacktes Spiel). zstd-Dictionary (§4). KeyDerivation bleibt nur für den CLI-`--secret`-Pfad; der Engine-Export nutzt einen Random-Key.

---

## 1. Ist-Zustand (v1)

### 1.1 Die komplette Pipeline heute

```
Quell-Assets (.png/.jpg/.tga/.hdr, .gltf/.glb, .hmat, .wav)
   │
   │  asset_compiler.exe        src/HE_Tools/src/AssetCompiler/main.cpp
   │  (+ HorizonImporters: Texture/Mesh/SkeletalMesh/Material/Audio)
   ▼
.hasset-Dateien             (HAsset-Binärformat, spiegelt Quell-Baum)
   │
   │  hpak_packer.exe   ODER   Editor „Build ▸ Export Project"
   │  src/HE_Tools/src/Packer/main.cpp   |   ProjectExporter::exportProject
   ▼
Export-Ordner:
   ├─ <Projekt>.hpak      alle .hasset-Blobs, per-Entry LZ4 + XOR (optional)
   ├─ project.hcfg        beschreibt Pak + Startup-Szene (HCFG-Format)
   ├─ <Main>.hescene      LOSE als JSON kopiert (nicht im Pak, nicht binär)
   └─ Game-Runtime        HorizonGame-Executable + dylibs/DLLs (kopiert)
   │
   ▼
HorizonGame-Runtime         src/HE_Game/src/GameApplication.cpp
   • OnInit: liest project.hcfg aus dem exe-Verzeichnis
   • setContentRoot(exeDir/Content)
   • contentManager().loadPak(pakPath)        ← OHNE Key!
   • loadPak: enumerate() + readEntry() je Asset → loadAssetFromMemory
   • Startup-Szene aus JSON laden (SerializeFormat::JSON)
```

### 1.2 HAsset (`.hasset`) — der Per-Asset-Container (Unterbau, bleibt)

Definiert in [HAsset.h](../src/HE_Core/include/ContentManager/HAsset.h). Ein `.hasset` ist ein Chunk-Container:

```
[ FileHeader (32B) ][ ChunkHeader(12B)+data ] × chunk_count
FileHeader: magic "HAST", version 2, asset_type(u16), chunk_count(u32), flags(u32 reserved), reserved[16]
ChunkHeader: id(u32 4CC), size(u64)
```

Die `META`-Chunk trägt Typ + **UUID** (16B) — daraus zieht der Packer die Entry-ID
([HpakWriter.cpp:10 `uuidFromHasset`](../src/HE_Core/src/Hpak/HpakWriter.cpp)). Chunks z.B. `VERT/INDX/NORM/TEXC`
(Mesh), `PIXL/TXMI` (Textur), `PCMD/AUMI` (Audio), `SRC ` (Script/Shader), `SCNE` (Szene).
**Der `.hasset`-Layer bleibt in v2 unverändert** — hpak ist nur der Archiv-Layer darüber.

### 1.3 hpak (`.hpak`) v1 — Format

[HpakFormat.h](../src/HE_Core/include/Hpak/HpakFormat.h):

```
[ FileHeader (16B) ][ EntryDesc(36B) × entryCount ][ rohe Datenblöcke ]

FileHeader (16B):  magic[4]="HPAK", version(u32)=1, entryCount(u32), flags(u32 reserved)
EntryDesc  (36B):  uuidHi(u64), uuidLo(u64), origSize(u32), dataSize(u32),
                   dataOffset(u64), entryFlags(u8), pad[3]

entryFlags: kFlagCompressed 0x01 (LZ4), kFlagEncrypted 0x02 (XOR)
Reihenfolge: compress → encrypt (write) / decrypt → decompress (read)
```

- **Kompression:** LZ4, opt-in per Entry. Genutzt wird `LZ4_compress_default` — der **schwächste** Modus
  ([HpakWriter.cpp:44](../src/HE_Core/src/Hpak/HpakWriter.cpp)). Fallback auf „uncompressed" bei Fehler oder ohne LZ4.
- **„Verschlüsselung":** repeating-32-Byte-**XOR** ([HpakWriter.cpp:66](../src/HE_Core/src/Hpak/HpakWriter.cpp),
  [HpakReader.cpp:72](../src/HE_Core/src/Hpak/HpakReader.cpp)). Key via **PBKDF2-HMAC-SHA256 mit genau 1 Iteration**
  ([KeyDerivation.cpp:115](../src/HE_Core/src/Hpak/KeyDerivation.cpp)), SHA-256/HMAC handgerollt. Das CLI-Tool nutzt
  **Zero-Salt** ([Packer/main.cpp:46](../src/HE_Tools/src/Packer/main.cpp)).
- **`project.hcfg`:** eigenes „HCFG"-Format ([ProjectConfig.cpp](../src/HE_Core/src/Hpak/ProjectConfig.cpp)):
  magic, version, `projectName`, `hpakFilename`, `mainSceneName`, `projectUuidBytes[16]`, `flags` (Bit0 = `enableModSupport`).

### 1.4 Verifizierte Defekte (primär, mit Zeilen)

| # | Defekt | Fundstelle | Auswirkung |
|---|---|---|---|
| D1 | **Game übergibt nie den Key** an `loadPak` | [GameApplication.cpp:64](../src/HE_Game/src/GameApplication.cpp) (`loadPak(pakPath)`, Default `key=nullptr`) | **Verschlüsselte Paks sind zur Laufzeit nicht ladbar** — Feature funktionslos/vestigial. |
| D2 | **XOR ist keine Verschlüsselung** | HpakWriter.cpp:66 / HpakReader.cpp:72 | Repeating-Key-XOR ist **ohne** Key knackbar (Crib-Dragging auf bekannten `.hasset`/PNG/glTF-Headern). Hex-Editor genügt. |
| D3 | **KDF gebrochen** | KeyDerivation.cpp (1 Iteration), Packer/main.cpp:46 (Zero-Salt) | Selbst als Passwort-Schutz wertlos. |
| D4 | **O(n) Linear-Scan der TOC** | [HpakReader.cpp:39 `hasEntry`](../src/HE_Core/src/Hpak/HpakReader.cpp), :55 `readEntry` | Lookup je Asset O(n). |
| D5 | **fd-Reopen pro `readEntry`** | [HpakReader.cpp:59](../src/HE_Core/src/Hpak/HpakReader.cpp) (`std::ifstream f(m_path…)` in jeder Iteration) | `loadPak` = O(n²) Datei-Opens/Scans. |
| D6 | **Eager Whole-Pak in RAM** | [ContentManager.cpp:764 `loadPak`](../src/HE_Core/src/ContentManager/ContentManager.cpp) (enumerate + `loadAssetFromMemory` für **alle**) | Kein Streaming; ganzer Katalog beim Start dekomprimiert. |
| D7 | **Keine Integrität** | HpakFormat.h (kein Hash/CRC) | Korruptes/abgeschnittenes Pak wird nicht erkannt. |
| D8 | **Startup-Szene lose + JSON** | [ProjectExporter.cpp:38](../src/HE_Core/src/Hpak/ProjectExporter.cpp), [GameApplication.cpp:76](../src/HE_Game/src/GameApplication.cpp) | Nicht im Pak, nicht binär — widerspricht dem „Binary-Pfad"-Ziel der Roadmap. |
| D9 | **Async-Streaming ignoriert das Pak** | [ContentManager.cpp:236 `loadAssetAsync`](../src/HE_Core/src/ContentManager/ContentManager.cpp) liest nur **lose Dateien** von `relativePath` | Der 6.4-Worker-Pfad kennt Paks nicht. |

Was **schon gut** ist und bleibt: **per-Asset-Granularität** (eine `EntryDesc` pro `.hasset`, TOC-indiziert). Das
liefert bereits Random-Access auf Asset-Ebene — genau die Eigenschaft, die Unitys 128-KB-Chunking erst *synthetisiert*.
Unity chunkt, weil ein Bundle *viele* Assets in einen Blob packt; hpak nicht. Darum brauchen wir **kein** intra-Entry
Block-Framing als Default (siehe [§4](#4-kompression)).

---

## 2. hpak v2 — Zielformat

### 2.1 Gesamtlayout

```
┌───────────────────────────────┐  Offset 0
│ FileHeader (64 B)             │
├───────────────────────────────┤  64
│ EntryDesc (56 B) × entryCount │  TOC, aufsteigend nach UUID sortiert
├───────────────────────────────┤
│ [optional] zstd-Dictionary    │  dictOffset / dictSize im Header
├───────────────────────────────┤
│ Datenblöcke @ dataOffset      │  je Entry: [compress →] [encrypt →] Bytes (+GCM-Tag)
└───────────────────────────────┘
Alle Werte little-endian. #pragma pack(1). static_assert auf jede Struct-Größe.
```

### 2.2 `FileHeader` v2 (64 Byte)

```cpp
#pragma pack(push, 1)
struct FileHeader                 //  Offset
{
    char     magic[4];            //  0   "HPAK"
    uint32_t version;             //  4   = 2
    uint32_t entryCount;          //  8
    uint32_t flags;               // 12   Archiv-Flags: kArchiveSortedTOC|kArchiveHasDict|kArchiveEncrypted
    uint64_t buildId;             // 16   XXH3 des Builds / semver-Stempel  → Patch-Validierung
    uint64_t baseArchiveId;       // 24   0 = Basis-Pak; sonst buildId der Basis (Overlay/Patch)
    uint64_t tocHash;             // 32   XXH3-64 über die EntryDesc-Region  → fail-fast beim Mount
    uint64_t dictOffset;          // 40   Offset des shared zstd-Dictionary-Blobs (0 = keins)
    uint32_t dictSize;            // 48
    uint32_t reserved0;           // 52
    uint64_t reserved1;           // 56
};                                // 64
static_assert(sizeof(FileHeader) == 64, "Hpak::FileHeader must be 64 bytes");
#pragma pack(pop)
```

### 2.3 `EntryDesc` v2 (56 Byte)

```cpp
#pragma pack(push, 1)
struct EntryDesc                  //  Offset
{
    uint64_t uuidHi;              //  0   ← TOC ist nach (uuidHi,uuidLo) sortiert
    uint64_t uuidLo;              //  8
    uint64_t dataOffset;          // 16   Byte-Offset ab Dateianfang
    uint32_t origSize;            // 24   unkomprimierte .hasset-Größe
    uint32_t dataSize;            // 28   gespeicherte Größe (comp+enc, inkl. 16B GCM-Tag falls verschlüsselt)
    uint64_t contentHash;         // 32   XXH3-64 der GESPEICHERTEN Bytes  → Integrität beim Read + Delta-Patching
    uint8_t  nonce[12];           // 40   AEAD-Nonce (96 bit); 0 wenn unverschlüsselt
    uint8_t  codec;               // 52   0=store, 1=lz4, 2=zstd
    uint8_t  entryFlags;          // 53   kFlagEncrypted | kFlagUsesDict | kFlagBlockFramed(reserviert)
    uint16_t pad;                 // 54
};                                // 56
static_assert(sizeof(EntryDesc) == 56, "Hpak::EntryDesc must be 56 bytes");
#pragma pack(pop)

// Archiv-Flags (FileHeader::flags)
inline constexpr uint32_t kArchiveSortedTOC = 0x1;  // TOC nach UUID sortiert (Binärsuche erlaubt)
inline constexpr uint32_t kArchiveHasDict   = 0x2;  // dictOffset/dictSize gültig
inline constexpr uint32_t kArchiveEncrypted = 0x4;  // mind. ein Entry ist verschlüsselt

// Entry-Flags (EntryDesc::entryFlags)
inline constexpr uint8_t  kFlagEncrypted    = 0x1;  // AES-256-GCM (siehe §5 — Obfuskation, kein Schutz!)
inline constexpr uint8_t  kFlagUsesDict     = 0x2;  // zstd-Entry mit Archiv-Dictionary komprimiert
inline constexpr uint8_t  kFlagBlockFramed  = 0x4;  // RESERVIERT: per-Block-Framing (noch nicht implementiert)
```

**Warum genau diese Felder** (jeweils: macht die Industrie das? lohnt es *hier*?):

| Feld | Zweck | UE / Unity | Hier lohnend? |
|---|---|---|---|
| `codec` (statt Flag-Bit) | store/lz4/zstd sauber unterscheidbar (zwei Codecs) | UE: method-index-Tabelle | **ja** — wir haben zwei Codecs |
| `contentHash` (XXH3-64) | Read-Integrität + quasi-gratis Delta-Patching | UE SHA1, Unity CRC32 | **ja, wichtigste strukturelle Änderung** — XXH3 ist schneller als `memcpy` (~31 GB/s) |
| `tocHash` (Header) | Fail-fast bei Truncation/Korruption beim Mount | UE IndexHash (SHA1) | **ja** — quasi gratis |
| `buildId` / `baseArchiveId` | Patch-Validierung + Overlay-Targeting | UE build-id / mount-priority | **ja** — 16 B jetzt, teuer nachzurüsten; Overlay-Code kommt später |
| `nonce[12]` | AEAD pro Entry (Nonce-Reuse = Two-Time-Pad-Bruch!) | UE EncryptionKeyGuid | **ja** — die Verschlüsselung braucht es (§5) |
| `kFlagBlockFramed` | Haken für optionales per-Block-Streaming großer Assets | UE/Unity: ja (Multi-Asset-Blobs) | **nur Bit reservieren**, Code ist YAGNI (§4) |

**Zwei getrennte Integritäts-Ebenen** (beides billig, beides fehlt heute):
`tocHash` beim Mount (fail-fast) + `contentHash` beim Read (lokale Korruption / Streaming). Bei verschlüsselten
Entries deckt der GCM-Auth-Tag die Integrität ohnehin ab; `contentHash` bleibt für `store`/`lz4`/`zstd`-Entries wichtig.

**Explizit übersprungen:** SHA-256 + RSA-Signatur (10× langsamer als XXH3; keine ausgelieferten Titel = keine
Tamper-Oberfläche), Perfect-Hash-TOC (Binärsuche reicht weit jenseits unserer Katalog-Größe), Partitionierung
(Multi-`.ucas`-Splitting für Plattform-Dateigrößen-Limits — brauchen wir nicht).

### 2.4 Alignment

**Default: keins.** Entries sind komprimiert+verschlüsselt → der Reader allokiert und alignt den Decode-Ziel-Buffer
selbst; In-Archive-Alignment brächte nichts. **Nur** falls je rohe, GPU-fertige BCn-Blobs (`codec=store`) gespeichert
werden: `dataOffset` auf **256** (D3D12 Pitch/CB) bzw. **512** (Texture-Placement) padden, damit der mmap-Pointer ohne
Realign-Copy hochgeladen werden kann. Das ist eine Zeile im Writer, kein Feature — bei Bedarf.

### 2.5 `project.hcfg` v2

Ergänzen (Version bump auf 2):

```
+ buildId (u64)           // muss dem Pak-buildId entsprechen (Mismatch → Warnung)
+ startupSceneUuid[16]    // Startup-Szene per UUID aus dem Pak (statt loser Dateiname)
+ encKey[32]              // zufälliger 256-bit-AES-Key (siehe §5; ehrlich: Obfuskation)
  (mainSceneName bleibt optional als Fallback für lose Szenen im Dev-Modus)
```

Der `encKey` reist bewusst **im hcfg mit** — der generische `HorizonGame`-Runtime ist projekt-unabhängig, ein
Compile-In pro Projekt ist nicht möglich. Das ist die ehrliche Konsequenz der Krypto-Weiche (§5).

---

## 3. Packen (Build-Pipeline)

### 3.1 Ablauf v2

```
asset_compiler   Quelle → .hasset            (unverändert; nur binäre Szenen-Serialisierung ergänzen, s.u.)
      ▼
HpakWriter::addDirectory / addEntry          (rekursiv alle *.hasset, UUID aus META)
      │  je Entry:
      │    1. codec wählen (Editor: lz4-fast; Ship: zstd -19 [+ Dict])
      │    2. komprimieren  (LZ4_compress_HC / ZSTD_compress[_usingCDict])
      │    3. verschlüsseln (optional, AES-256-GCM, zufällige Nonce → EntryDesc.nonce)
      │    4. contentHash = XXH3(gespeicherte Bytes)
      ▼
HpakWriter::write
      1. [Ship] zstd-Dictionary aus allen .hasset-Samples trainieren (ZDICT) → Dict-Blob
      2. TOC nach UUID SORTIEREN
      3. Offsets berechnen (Header → TOC → [Dict] → Datenblöcke)
      4. tocHash = XXH3(TOC-Region); buildId setzen
      5. schreiben
```

**Determinismus** (für Delta-Patching): gleiche Quellen + gleiche Settings → byte-identisches Pak. Sortierte TOC hilft
dabei bereits; `LZ4_compress_HC`/`ZSTD_compress` sind deterministisch. Ein Patcher diffed dann `contentHash` je UUID
und liefert nur geänderte Entries in einem Overlay-Pak (`baseArchiveId` = buildId der Basis) aus.

### 3.2 Startup-Szene ins Pak (D8 schließen)

Szenen binär als `.hasset` serialisieren (`SceneSerializer` Binary-Pfad, Roadmap-Ziel) und **mit ins Pak** packen;
`project.hcfg` referenziert die Startup-Szene per `startupSceneUuid` statt loser JSON-Datei. Beseitigt die
JSON/lose-Inkonsistenz und macht das Export-Ergebnis ein einziges, konsistentes Artefakt.

### 3.3 `ExportSettings` / CLI erweitern

```cpp
struct ExportSettings {
    enum class Codec { Store, LZ4Fast, Zstd } codec = Codec::Zstd;
    int  zstdLevel   = 19;      // Ship; Editor-Iteration nutzt LZ4Fast
    bool trainDict   = true;    // zstd: shared Dictionary trainieren
    bool encrypt     = false;   // AES-256-GCM (Obfuskation)
    // key wird beim Export ZUFÄLLIG generiert und in project.hcfg geschrieben (nicht mehr abgeleitet)
    std::filesystem::path gameRuntimeDir;   // wie bisher
};
```

`hpak_packer` CLI analog: `--codec {store|lz4|zstd} --level N --dict --encrypt` (Key wird generiert und neben das Pak
als hcfg geschrieben; die `--secret`-Passphrase-Ableitung entfällt, siehe §5).

---

## 4. Kompression

Zentrale Erkenntnis: **hpak ist bereits per-Asset-random-access** — wir brauchen kein Whole-Archive-Format und kein
intra-Entry-Chunking als Default.

| Entscheidung | Plan | Begründung |
|---|---|---|
| **Sofort-Gewinn:** `LZ4_compress_default` → `LZ4_compress_HC(…, 9)` | **ja, sofort** (Einzeiler [HpakWriter.cpp:44](../src/HE_Core/src/Hpak/HpakWriter.cpp)) | `lz4hc.c` ist bereits im Build ([CMakeLists.txt:172-Bereich](../CMakeLists.txt)), Homebrew-liblz4 hat es immer. Ratio ~2.10× → ~2.72×, **identischer Decode-Pfad und -Speed** (`LZ4_decompress_safe` dekodiert HC-Blocks unverändert). Kein Format-Bump, keine neue dylib. Pack ist offline → langsamer HC-Encode egal. |
| **Ship-Codec:** zstd | **ja** (deine Wahl) | Level 1 ~2.9× schlägt LZ4HC schon; -19 ~3.5×. Decode ~1100–1500 MB/s über *alle* Level konstant → Level rein nach Pack-Budget wählen (compress-once). Neue C-Lib via FetchContent (spiegelt lz4-Setup) + eine dylib + Format kennt `codec=2`. |
| **zstd-Dictionary** | **ja** (Ship, `trainDict`) | Der eigentliche Ratio-Sprung bei vielen kleinen ähnlichen `.hasset` (Prefabs/Materials/Scenes): Metas Small-Record-Beispiel 2.8× → 6.9×. Ein **shared** Dictionary holt Whole-Archive-Cross-File-Gewinne, **ohne** den per-File-Random-Access zu opfern. Dict-Blob im Pak (`dictOffset/dictSize`), Reader lädt es einmal beim Mount. |
| **Whole-Entry vs per-Block** | **Whole-Entry Default; `kFlagBlockFramed` nur reservieren** | Per-Block hilft nur bei einzelnen Multi-MB-Assets (Streaming-Texturen/Audio). Implementierung (per-Entry-Liste von {compSize,uncompSize}-Blöcken à 64–128 KB) erst, wenn ein reales Asset es erzwingt. YAGNI. |
| `--ultra` -20…-22 | **skip** (außer finale Ship-Builds) | Riesige Encode-Zeit/RAM für wenige % über -19. |
| Oodle, LZMA, LZ4-Frame-API, zstd-Seekable | **skip** | Oodle lizenziert; LZMA verliert Random-Access (Unity wirft es im Cache selbst weg); Frame/Seekable lösen ein Random-Access-Problem, das unsere per-Entry-TOC nicht hat. |

**Messung als Gate:** Vor dem zstd-Aufwand reale `.hpak`-Größen mit LZ4HC-9 messen; zstd nur ziehen, wenn LZ4HC
spürbar Download/Footprint auf dem Tisch lässt. (Die Weiche ist getroffen — der Messschritt bestätigt nur Level/Dict-Nutzen.)

---

## 5. Verschlüsselung — ehrliches Threat-Model

> **Befund zuerst:** Jedes Schema, bei dem der Client seine eigenen Assets entschlüsselt, ist **Obfuskation gegen
> casual Ripping, nie eine Sicherheitsgrenze** — der Key muss im Client liegen. Da `HorizonGame` (generisch) und
> `project.hcfg` (mit Key) **zusammen** ausgeliefert werden, ist der Key trivial auffindbar. UnrealKey zieht AES-256
> aus laufenden UE-Spielen in Sekunden; UnityCN-Helper reversed Unitys baked-in Bundle-Krypto. **Wir werden es nicht
> besser machen als Epic.** Ziel ist nur, die Latte von „Hex-Editor / Ripper" auf „Debugger + RAM-Dump" zu heben.

Da du dennoch **AES-256** willst, ist der einzige verteidigbare Weg:

- **AES-256-GCM (AEAD) via vetted Library — nicht selbst rollen.** Wir haben bereits SHA-256/HMAC/PBKDF2 handgerollt
  ([KeyDerivation.cpp](../src/HE_Core/src/Hpak/KeyDerivation.cpp)); genau das aufhören. **libsodium** (FetchContent):
  `crypto_aead_aes256gcm_*` (HW-AES vorhanden) mit `crypto_aead_xchacha20poly1305_*` als portablem Fallback.
  Der Auth-Tag gibt **Tamper-Detection** — für einen Solo-Dev nützlicher als reine Confidentiality.
- **Per-Entry-Zufalls-Nonce** (das `nonce[12]`-Feld). **Pflicht:** Nonce-Reuse unter einem Key reproduziert exakt die
  Two-Time-Pad-Schwäche des alten XOR. Der 16-B-GCM-Tag wird an den Ciphertext angehängt (`dataSize` inkl. Tag).
- **KDF komplett droppen.** Wenn der Key ohnehin mitreist, bringt Ableitung nichts → **zufälligen 256-bit-Key beim
  Export generieren**, in `project.hcfg` schreiben ([§2.5](#25-projecthcfg-v2)). `KeyDerivation.*` und die Zero-Salt/1-Iter-PBKDF2
  entfallen ersatzlos; die öffentliche Projekt-UUID als Salt verschwindet.
- **Reihenfolge:** compress → encrypt (write) / decrypt → decompress (read) — wie heute.
- **Ehrliche Doku:** Kommentar im Format-Header: *„kFlagEncrypted raises the bar against casual ripping — it is not a
  security guarantee. The key ships with the game."* Schützt künftige Contributor vor Über-Vertrauen.

**D1 fixen:** `GameApplication` muss den Key aus `project.hcfg` an `mountPak(...)` übergeben — heute passiert das nie
([GameApplication.cpp:64](../src/HE_Game/src/GameApplication.cpp)).

**Skip:** echtes DRM (server-seitige Per-Session-Keys, Widevine L1 TEE, Denuvo). Das Einzige, das Content wirklich
schützt — braucht aber Lizenz-Server, Key-Exchange, Hardware-TEE, und schützt lokal entschlüsselte Single-Player-Assets
trotzdem nicht. Falsche Altitude. (UEs `RegisterEncryptionKey`-Muster ist das Studienobjekt, *falls* je Live-Service.)

---

## 6. Runtime-Loading in HorizonGame

Drei getrennte Defekte (D4, D5, D6) — getrennt lösen.

### 6.1 `HpakArchive` — gemapptes, indiziertes Archiv

Neue Klasse (ersetzt den heutigen zustandslosen `HpakReader`):

```cpp
class HpakArchive {
    // mount(): einmal mmap (POSIX mmap / Win MapViewOfFile), read()+pread-Fallback
    //   • Header validieren: magic, version==2, tocHash == XXH3(TOC-Region)  → fail-fast (D7)
    //   • Dictionary (falls kArchiveHasDict) einmal laden (ZSTD_createDDict)
    //   • Index = die gemappte, sortierte EntryDesc-Region SELBST (kein Parse, kein Alloc)
    // find(uuid): BINÄRSUCHE über die sortierte TOC  → O(log n)  (D4)
    // read(uuid): view auf mmap @ dataOffset  → [contentHash prüfen] → AEAD-decrypt → decompress
    //   • store + unverschlüsselt: Zero-Copy-Span direkt aus dem Mapping
    const uint8_t* m_map;  size_t m_size;   // Lebensdauer = Archiv-Lebensdauer (D5: kein fd-Reopen)
};
```

- **D4** → Binärsuche auf der sortierten In-File-TOC (allokationsfrei, mmap-fähig). `unordered_map<UUID,idx>` optional
  für O(1), kostet aber eine Mount-Allokation — Binärsuche reicht.
- **D5** → ein Mapping/Handle über die Lebensdauer; concurrent reads sind über mmap/`pread` thread-safe (wichtig für §6.3).
- **D7** → `tocHash` beim Mount, `contentHash` beim Read.

> **Ehrliches mmap-Caveat:** Zero-Copy greift nur bei `codec=store` **und** unverschlüsselt. Für lz4/zstd/AES
> allokiert der Reader ohnehin einen Decode-Buffer. Der reale Gewinn von mmap hier ist: kein fd-Reopen (D5),
> OS-Page-Cache managt Residency automatisch, kein manuelles seek/read-State-Machine. Volles Zero-Copy lohnt erst, wenn
> rohe BCn-Blobs gespeichert werden (dann auch In-Archive-Alignment §2.4).

### 6.2 ContentManager: eager → on-demand (D6)

Heute: [`loadPak`](../src/HE_Core/src/ContentManager/ContentManager.cpp) enumeriert und parst **alles**. Neu:

```cpp
bool  mountPak(path, key);            // öffnet HpakArchive, registriert UUID → {archive*, entryIdx}. PARST NICHTS.
                                      //   Overlay: mehrere Paks; höhere Mount-Priorität shadowt niedrigere (per UUID)
// acquireXxx(uuid):  falls nicht resident → aus gemountetem Archiv lesen+parsen, registrieren, pinnen
```

- **Residency ref-counted** (nutzt das bestehende `AssetRef<T>` / `pinAsset`/`unpinAsset`,
  [ContentManager.h:117](../src/HE_Core/include/ContentManager/ContentManager.h)). **Regel von Addressables stehlen:
  Unload-Granularität = Load-Granularität.** hpak hat hier einen strukturellen Vorteil: per-Entry-Granularität
  **vermeidet Unitys „ein lebendes Asset pinnt das ganze Bundle in RAM"-Footgun** — bei uns ist die Unload-Einheit
  natürlich feiner (per Asset). Explizit dokumentieren.
- **Overlay/Mod-Support:** `enableModSupport` (heute ungenutztes hcfg-Flag) verdrahten: Mods-Verzeichnis scannen,
  Overlay-Paks mit passender `baseArchiveId` über die Basis mounten; gleiche UUID = Replacement, neue UUID = Addition
  (Quake/UE-Modell: „later mount shadows earlier").

### 6.3 Async-Streaming-Integration (D9)

Der 6.4-Worker-Pfad ([loadAssetAsync](../src/HE_Core/src/ContentManager/ContentManager.cpp) / `pollAsyncResults`) liest
heute nur lose Dateien. Erweitern:

- `loadAssetAsync(uuid, cb)`: Worker liest Entry-Bytes aus dem gemounteten `HpakArchive` (mmap/`pread` — concurrent
  read-safe), **decrypt + decompress auf dem Worker** (CPU-teuer, gehört vom Main-Thread weg), reicht die entpackten
  `.hasset`-Bytes über die bestehende `AsyncResult`-Queue an den Main-Thread.
- `pollAsyncResults` parst + registriert wie gehabt auf dem Main-Thread (keine Locks auf den SlotMaps nötig).
- `AsyncResult` bekommt eine UUID-Variante (statt nur `relativePath`): `{ archive*, entryIdx }` oder direkt der
  entpackte Byte-Buffer.

### 6.4 GameApplication

- Key aus `project.hcfg` an `mountPak` übergeben (D1). ✅
- Startup-Szene per `startupSceneUuid` aus dem Pak laden (statt lose JSON, D8). ✅
- Rest (`OnRender`-System-Tick) unverändert.

---

## 6.5 Referenzgraph — nur gebrauchte Assets laden ✅ UMGESETZT (Step 1+2)

> **Status:** Beide Stufen gebaut und getestet (506 Tests grün, Metal-Headless-Shot verifiziert).
> **Step 1**: Pack-Zeit-Baking der Pfad-Refs zu UUIDs (Chunks `MRFU`/`MTLU`/`SCNU`), Companion-Fix
> (echter META-Pfad in `m_pathToUUID`), Frontier-Expansion in `pollAsyncResults`, Seed via
> `SceneSystems::collectAssetRefs` — GameApplication streamt nur noch die Szenen-Closure.
> **Step 2 („nur UUIDs")**: Der Packer **droppt die Pfad-Strings** im Pak (MREF/SCNE ersetzt,
> MTRL mit leeren Strings + byte-verbatim PBR-Tail), und die Backend-Resolver laufen im
> Dual-Mode über zentrale Helper `ContentManager::resolveMaterialRef/resolveTextureRef`
> (baked UUID bevorzugt → `ensureResident`, Editor-Pfad als Loose-Fallback). Umgestellt:
> Metal ×3, GL ×3 (beide kompiliert + Metal visuell verifiziert), **D3D11 ×2 blind**
> (Windows-Verify offen). Lose Editor-Assets bleiben vollständig pfad-basiert (Debugging).
> Der ursprüngliche Design-Text folgt unverändert als Begründungs-Doku.

### (ursprüngliches Design)

**Ziel:** statt `streamMountedAssets()` das ganze Pak zu streamen, nur die Assets laden, die die
Startup-Szene tatsächlich referenziert (+ deren transitive Abhängigkeiten). Speicher-/Bandbreiten-Optimierung,
keine Korrektheitsfrage.

### Die eigentliche Hürde (Ursachenanalyse)

Nicht „es fehlt ein Index" — sondern: **Intra-Asset-Referenzen sind als PFADE gespeichert**, nicht als UUIDs.
- Szene → Asset: **schon UUID-basiert** (`MeshComponent.meshAssetId`, `MaterialComponent.materialAssetId`,
  `SkeletalMeshComponent.meshAssetId`, `AudioSourceComponent.assetId`, `AnimatorComponent.clipAssetId`, …
  ~18 Komponenten-Typen). Die *erste* Ebene ist also trivial: Szene-Registry durchgehen, referenzierte UUIDs sammeln.
- Asset → Asset: **pfad-basiert** (`MaterialAsset.texturePaths` = Strings, `StaticMeshAsset.materialPath` = String)
  und wird zur Laufzeit über `ContentManager::loadAsset(path)` aufgelöst — das **von der Content-Root-Platte liest**
  ([MetalRenderer.mm ResolveMaterialTexture](../src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm) ruft
  `loadAsset(mat->texturePaths[0])`). Im ausgelieferten Spiel gibt es keine losen Dateien → das funktioniert heute
  nur, weil *streaming-all* nach und nach `m_pathToUUID` füllt und `loadAsset(path)` dann den Cache trifft.

→ **„Nur Gebrauchtes laden" bricht genau diesen Selbstheilungs-Mechanismus.** Sobald ein Material geladen wird,
das eine noch nicht gestreamte Textur (per Pfad) referenziert, schlägt `loadAsset(texturePath)` fehl (Platte leer im
Ship-Build) → Textur fehlt. Man braucht also **Pfad→UUID-Auflösung beim Mount, ohne alles zu dekodieren.**

### Drei Optionen (Pfad→UUID / Dependency-Auflösung)

| Option | Was | Aufwand | Bewertung |
|---|---|---|---|
| **(a) Pfad→UUID-Manifest im Pak** | Der Packer liest ohnehin jede `.hasset`-META (für die UUID) — er schreibt zusätzlich eine `path→UUID`-Tabelle als Sonder-Entry/Header-Region. Mount lädt sie → `ContentManager` hat einen vollständigen Pfad-Index OHNE Assets zu dekodieren. `loadAsset(path)` löst dann über das Manifest auf → `ensureResident`/stream statt Platte. | **klein** — Packer + ein Format-Feld + Mount-Load + `loadAsset`-Umleitung | **empfohlen** — pragmatisch, löst auch das Ship-Build-`loadAsset(path)`-Problem generell |
| **(b) Gebackene per-Entry-Dependency-UUIDs** | Packer löst zur Pack-Zeit jede Intra-Ref (Pfad) → UUID auf und speichert die Dep-UUID-Liste je Entry im TOC/Manifest. Laufzeit-Closure = reine UUID-Traversierung, keine Pfad-Indirektion. | **mittel** — Packer muss Refs extrahieren+auflösen, TOC/Manifest-Erweiterung | sauberer als (a) für die Closure, aber mehr Pack-Logik; baut auf (a) auf |
| **(c) Intra-Refs im `.hasset`-Format auf UUIDs umstellen** | `materialPath`/`texturePaths` → `materialUuid`/`textureUuids` überall (Importer, Serialisierung, Runtime-Resolver). Behebt die Ursache global. | **groß** — berührt Importer + ContentManager + jede Resolve-Stelle in allen Backends | „richtig", aber der größte Eingriff; Langfrist-Ziel |

### Closure-Entdeckung — Frontier-Expansion über `pollAsyncResults`

Komponiert sauber mit dem bereits Gebauten, egal welche Option:
1. **Seed:** Szene-Registry durchgehen, alle Komponenten-Asset-UUIDs sammeln → `loadAssetAsync(uuid)` für jede.
2. **Expansion:** in `pollAsyncResults`, wenn ein Asset registriert wird, seine Intra-Refs inspizieren
   (Material→`texturePaths`, Mesh→`materialPath`) → via Manifest (a/b) → UUID → falls neu: `loadAssetAsync(uuid)`.
3. Wiederholen bis die Frontier leer ist. Der Graph wird also **inkrementell über Frames** entdeckt, getrieben von
   der bestehenden Drain-Schleife — kein separater Vorab-Graph nötig (bei Option a). Bei Option (b) ist die Closure
   schon im Manifest → man kann sie in einem Rutsch seeden.

### Sicherheitsnetz

Verfehlt die Closure eine Ref (nicht erfasster Referenz-Typ), fehlt im Ship-Build ein Asset (kein Crash — der
Renderer überspringt Nicht-Residentes, aber sichtbar fehlend). Zwei Absicherungen: (1) `loadAsset(path)` **muss** im
Ship-Build über das Manifest auf `ensureResident` umgeleitet werden (nicht Platte) — dann heilt ein verfehltes,
aber später angefragtes Asset sich selbst; (2) optional ein „stream-all nach idle"-Fallback (erst die Closure, dann
im Hintergrund den Rest) → garantiert Vollständigkeit ohne den Start-Latenz-Gewinn zu opfern.

### Empfehlung / Reihenfolge

**(a) Manifest + Frontier-Expansion**, plus `loadAsset(path)`→Manifest-Umleitung als Pflicht-Begleitfix. Das ist der
kleinste Schritt, der „nur Gebrauchtes" ermöglicht UND das latente Ship-Build-`loadAsset(path)`-Loch schließt.
(b) später, wenn die Pack-Zeit-Dep-Extraktion sich lohnt; (c) als Langfrist-Formatbereinigung. Geschätzter Aufwand
für (a): Packer-Manifest + Mount-Load + `loadAsset`-Umleitung + Szene-Seed + `pollAsyncResults`-Expansion + Tests —
ein fokussierter eigener Durchgang, nicht in diesen Diff.

---

## 7. Was wir explizit NICHT bauen

| Skip | Industrie? | Warum hier nicht |
|---|---|---|
| Perfect-Hash-TOC (UE Seeds+Overflow) | UE | Binärsuche reicht weit über unsere Katalog-Größe; fehleranfällig. |
| IoDispatcher / Zen-Loader + Export-Bundle-Dependency-Flattening | UE | Offline-Graph-Machinerie für Millionen-Asset-Streaming. Falsche Altitude. |
| Oodle | UE default | Lizenziert; LZ4HC/zstd deckt ab. |
| SHA-256 + RSA-Signatur | UE | 10× langsamer als XXH3; keine Tamper-Oberfläche. |
| Remote-Catalog / OTA-Content-Update-Pipeline | Unity | Für ausgelieferte Titel mit OTA. `contentHash` macht simples Delta-Patching schon fast gratis; revisit beim echten Ship. |
| LZMA / Whole-Archive-Kompression | Unity (LZMA) | Verliert Random-Access; Unity wirft es im Cache selbst weg. |
| Provider/AsyncOperationHandle/IResourceLocation-Indirektion | Unity | Lohnt in General-Purpose-Engines mit pluggable Sources; wir rufen `ContentManager` direkt. |
| Partitionierung (Multi-`.ucas`) | UE | Für Plattform-Dateigrößen-Limits; brauchen wir nicht. |

---

## 8. Roadmap / Milestones

**M1 — Format v2 + Sofort-Gewinne** (Layout jetzt gratis)
1. `version → 2`; neue `FileHeader`(64B)/`EntryDesc`(56B) + `static_assert`; XXH3 vendored (`xxhash.h`, header-only, keine dylib).
2. TOC beim Schreiben nach UUID sortieren; `tocHash`/`contentHash`/`buildId` schreiben.
3. `LZ4_compress_default` → `LZ4_compress_HC(9)` (Einzeiler, kein Bump nötig — gleich mitnehmen).
4. `nonce`/`codec`/Flag-Bits reservieren.

**M2 — Kompression: zstd + Dictionary**
5. zstd via FetchContent (spiegelt lz4); `codec=2` in Writer/Reader; Level 19 Ship.
6. Dictionary-Training (ZDICT) in `HpakWriter::write`; Dict-Blob im Pak; Reader lädt `DDict` beim Mount.
7. `ExportSettings`/CLI um Codec/Level/Dict erweitern.

**M3 — Verschlüsselung: AES-256-GCM**
8. libsodium via FetchContent; XOR → AEAD, per-Entry-Nonce; `KeyDerivation.*` löschen.
9. Zufälligen 256-bit-Key beim Export generieren → `project.hcfg`; Game übergibt Key an `mountPak` (**D1**).
10. Ehrlicher Doc-Kommentar im Format-Header.

**M4 — Runtime-Loading-Umbau**
11. `HpakArchive`: mmap + persistenter Handle, Binärsuche, `tocHash`-Mount-Check (**D4/D5/D7**).
12. `ContentManager`: `mountPak` (lazy) statt eager `loadPak`; on-demand Parse via `acquire`; refcounted Residency (**D6**).
13. Async-Worker liest+entpackt aus dem Pak (**D9**).
14. Overlay-Mounting nach Priorität → `enableModSupport` verdrahten.
15. Startup-Szene binär ins Pak; hcfg referenziert per UUID (**D8**).

**M5 — Build-Pipeline + Tests**
16. `asset_compiler → hpak_packer`-Kette in ein Game-Export-Skript (analog [package_macos.sh](../scripts/package_macos.sh), das aktuell nur den Editor bündelt).
17. Deterministisches Packen verifizieren (Delta-Patching-Voraussetzung).
18. Tests erweitern ([test_hpak.cpp](../tests/test_hpak.cpp), [test_project_exporter.cpp](../tests/test_project_exporter.cpp)):
    v2-Roundtrip, `tocHash`/`contentHash`-Korruptions-Reject, AES-GCM-Roundtrip + Tamper-Reject, zstd(+Dict)-Roundtrip,
    Overlay-Shadowing, on-demand + async Laden.

**Gate auf Messung / echten Bedarf**
19. zstd-Level-Feintuning + `--ultra` nur final. 20. Optionale block-framed Entry-Variante (erst bei realem Multi-MB-Asset).

---

## 9. Referenz — betroffene Dateien

| Datei | Änderung |
|---|---|
| [HpakFormat.h](../src/HE_Core/include/Hpak/HpakFormat.h) | v2-Structs, Flags, `codec`, `static_assert` |
| [HpakWriter.cpp](../src/HE_Core/src/Hpak/HpakWriter.cpp) | LZ4HC/zstd/Dict, AES-GCM, sortierte TOC, Hashes (Zeile 44: Codec-Wechsel) |
| [HpakReader.cpp](../src/HE_Core/src/Hpak/HpakReader.cpp) | → `HpakArchive`: mmap, Binärsuche, Hash-Checks, AEAD (Zeilen 39/55/59/82) |
| [KeyDerivation.cpp/.h](../src/HE_Core/src/Hpak/KeyDerivation.cpp) | **löschen** (Key wird generiert, nicht abgeleitet) |
| [ProjectConfig.*](../src/HE_Core/src/Hpak/ProjectConfig.cpp) | hcfg v2: `buildId`, `startupSceneUuid`, `encKey` |
| [ProjectExporter.cpp](../src/HE_Core/src/Hpak/ProjectExporter.cpp) | Codec/Dict/Key-Gen, binäre Startup-Szene ins Pak |
| [ContentManager.*](../src/HE_Core/src/ContentManager/ContentManager.cpp) | `mountPak` lazy, on-demand `acquire`, Async-Pak-Pfad, Overlay |
| [GameApplication.cpp](../src/HE_Game/src/GameApplication.cpp) | Key an `mountPak` (D1), Startup-Szene per UUID (D8) |
| [Packer/main.cpp](../src/HE_Tools/src/Packer/main.cpp) | CLI-Flags (`--codec/--level/--dict/--encrypt`), `--secret` entfällt |
| [CMakeLists.txt](../CMakeLists.txt) | zstd + libsodium FetchContent (spiegelt lz4); xxhash vendored |

## 10. Quellen (Recherche)

UE `.pak`/IoStore: FPakInfo-Footer (magic `0x5A6F12E1`), Path-Hash-Index (V10+), 64-KB-Compression-Blocks, Oodle,
**AES-256-ECB mit gebackenem Key** (UnrealKey dumpt ihn in Sekunden), Mount-Priorität `_P`/numeric-patch für DLC
(repak/retoc, dev.epicgames.com). Unity: UnityFS-Blockdirectory, LZMA vs chunk-LZ4/LZ4HC (128 KB → on-demand),
Addressables-Katalog + `.hash`-Swap, ref-counted Bundle-Residency (docs.unity3d.com). Codecs: LZ4 ~2.10× / LZ4HC-9
~2.72× (identischer Decode ~4900 MB/s), zstd -1 ~2.9× / -19 ~3.5×, ZDICT 2.8×→6.9× (lz4/zstd GitHub, lzbench, Meta
Engineering). mmap/Alignment/Integrität: page-size 4096 / Win 64 KB granularity, D3D12 256/512, XXH3 ~31 GB/s vs SHA-256
~103 MB/s (man7, learn.microsoft.com, xxHash). Krypto: baked key = Obfuskation, AEAD-Nonce-Reuse = Two-Time-Pad, OWASP
PBKDF2 600k, Widevine L1 TEE als einzig echter Schutz (OWASP, libsodium/AES-GCM-Docs).
