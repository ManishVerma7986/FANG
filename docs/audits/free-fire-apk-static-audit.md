# Free Fire APK Static Audit

**Audit status:** Static metadata inspection completed on 2026-10-15. **Execution/testing remains BLOCKED.** The owner supplied a local XAPK path for static inspection only; the archive and APK contents were not installed or launched.

## Artifact identity and structure

The supplied filename is `Free+Fire+x+NARUTO+SHIPPUDEN_1.132.1_APKPure.xapk`. “APKPure” in the filename is not authenticated provenance and does not establish publisher authorization.

| Item | Verified value |
|---|---|
| XAPK format | XAPK v2 container |
| XAPK size | 408,164,795 bytes |
| XAPK SHA-256 | `90e7ce0d693eaf1ee06c1e3b3f27d39d57ba65630cb604e7de46592a8b4d60f3` |
| Package name | `com.dts.freefireth` |
| App label | Free Fire |
| Version name / code | `1.132.1` / `2019121229` |
| Minimum / target SDK | 21 / 36 |
| Included package components | `com.dts.freefireth.apk` (base), `asset_pack_install_time.apk` (install-time asset-pack split), `config.arm64_v8a.apk` (ABI config split), and `manifest.json` |
| Native ABI configuration | The supplied ABI split is `arm64-v8a`; no x86_64 ABI split is included. |

`aapt dump badging` confirmed the package identity, version, SDK bounds, and `arm64-v8a` split metadata. `apksigner verify --print-certs` succeeded for all three APK components. Each reports signer subject `CN=Android, OU=Android, O=Google Inc., L=Mountain View, ST=California, C=US`, certificate SHA-256 `ad820db4f0993eae2e109bae6965b3b41dbaf8c4ea6e794848e6719b36956c6d`, and source-stamp SHA-256 `3257d599a49d2c961a471ca9843f59d341a405884583fc087df4237b733bbd6d`. Signature verification establishes archive integrity relative to those certificates, not that the signer is Garena or that this artifact came from an authorized publisher channel. No independent official certificate fingerprint was available for comparison.

| APK component | SHA-256 |
|---|---|
| `com.dts.freefireth.apk` | `8563c8a701973c753e2f19b8c310dfd2453ae6c6a167ef375c2e193ebd702e1f` |
| `asset_pack_install_time.apk` | `a2c6989914498e63ccdb41491a0da48090ff460f9f6d723bb7e2d7aa37a0ab1` |
| `config.arm64_v8a.apk` | `27c1b8069a5226fc9ca8eac95ff057635b6c75a33e5e2065344edd09270583ce` |

The XAPK declares permissions/integrations including Internet/network state, location, camera, microphone, Bluetooth, storage, `com.google.android.gms.permission.AD_ID`, C2DM, Play install-referrer, and Play billing. The base manifest includes Google Play Services/Firebase-related metadata and a Firebase messaging service, but does not declare `com.google.android.gms` as a required shared library. These manifest declarations establish integration points, not whether Play services, network access, login, or another service is mandatory for startup or gameplay. That runtime dependency remains **unverified**; it was not tested.

## Compatibility with the currently approved reference image

The installed reference image package is API 36 `default` x86_64 revision 2. Its recorded `ro.dalvik.vm.native.bridge=0` does not advertise ARM native translation. This XAPK contains only an `arm64-v8a` configuration split and no x86_64 split. Therefore compatibility on the inspected x86_64 reference image is **unverified**: the package has no x86_64 ABI split and that image's recorded configuration does not advertise ARM native translation. This is not a runtime result and does not establish incompatibility on other systems or configurations. No AVD is currently configured, and no guest was booted for this audit.

The package also declares network and account/service integrations. The local package metadata cannot establish whether these services are required, and no connectivity or application behavior was tested. Google Play components were not added.

## Authorized-source and permission boundary

The filename is third-party-labeled, and its provenance was not independently authenticated. It is not evidence of an authorized package acquisition route. The existing [guest-execution decision record](../decisions/0001-guest-execution-strategy.md) documents the 2026-10-14 policy check: the official India Google Play route for original Free Fire (`com.dts.freefireth`) returned 404; the India Free Fire MAX listing is a distinct game/package and is not a substitute. Garena's accessible English Terms of Service clause 3.1 limits the license to “personal use only”; clause 4.1(h) prohibits commercial purpose or “the benefit of any third party.” Those clauses neither expressly permit nor specifically address Fang emulator compatibility testing. Applicable India permission and written publisher authorization remain unverified. No new official source or permission has been established by this audit.

Static inspection was limited to the supplied XAPK descriptor and Android package metadata/signatures. The XAPK was not installed, launched, executed, modified, or redistributed. No anti-cheat, licensing, or device-integrity control was bypassed. Do not use this artifact for runtime testing absent a publisher-authorized regional source, applicable permission, and separate explicit owner approval.

## Next decision

Per the owner's latest direction, Free Fire runtime testing is deferred and the game remains excluded as a representative workload. The inspected artifact's package provenance, applicable testing permission, runtime requirements, and compatibility on the approved x86_64 image remain unverified; no conclusion is made about compatibility on other systems. Do not install or launch it. The first-party animated GLES specification was approved on 2026-10-15 for graphics-path and instrumentation validation only; this does not authorize installation or launch, and the app remains unimplemented. Phase 9 remains **BLOCKED**; Phase 10 remains **NOT STARTED**.
