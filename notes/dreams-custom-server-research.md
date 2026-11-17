# Dreams custom-server feasibility research

Research snapshot: 2026-11-17. Working directory: `$REPO` (the OpenDream checkout).
Purpose: decide whether to fund a compatible replacement service, not document an implemented feature.

## Verdict and scope

**A compatible community server is technically plausible, but this is a substantial reverse-engineering project.**
A private, small-catalog service is a realistic first target. Exact equivalence to the official service is not established.
Reproducing its original catalog, accounts, social graph, and recommendation results requires data that protocol compatibility cannot create.

The user authorized this investigation and up to three concurrent DeepSeek workers.
This expands the earlier offline-only scope for research, not for implementation or live-service access.
No game run, production API probe, PSN login, new binary analysis, or server implementation occurred.
Existing audit/resolution changes remain untouched and uncommitted. Offline defaults remain unchanged.

Evidence labels in this report:
- **Observed:** public documentation or repository source inspected during this investigation.
- **Reported:** a third party describes a result that we did not reproduce.
- **Proposed:** architecture, validation, or engineering judgment, not implemented behavior.
- **Unknown:** evidence is missing. An endpoint name does not establish its request schema or semantics.

## 1. Existing research: useful, but not a ready-made server

| Source | Observed coverage | What it does not establish |
|---|---|---|
| [ArchivingDreams][S1] | Nine native service families and method paths derived from a decompiled game | Complete schemas, transport framing, authenticated responses, or a working replacement |
| [jaames/dreams-api][S2] | Website API proxy, metadata endpoints, ID codec, historical authentication/header handling | Native creation/blob downloads. Its README explicitly excludes level downloads |
| [dreams-api wiki][S3] | Website query parameters, genealogy, comments, CDN notes, partial blob-container description | Current native protocol or complete scene payload layout |
| [dreamiverse-transformer][S4] | README-only research, host inventory, file-format observations | Executable server, complete download flow, or completed creation backups |
| [Dreams-Exportation blob exporter][S5] | Code requests a configurable `/blob` URL | A real Dreams endpoint. The default host is `api.dreams.example.com`, a placeholder |
| Existing bbport online module | PS4 network/NP HLE and a separate-game custom-session integration pattern | Dreams support or compatibility with this port's default vcore build |

**No working Dreams PS4 replacement server or complete native message schema was found in the examined sources.**
The search covered indexed web/GitHub projects, native route strings, and preservation-community references.
Private, unlisted, or Discord-only work is not excluded. Contact existing researchers before duplicating their work.

`dreams-api` contains an MIT license, although its package metadata says ISC. Preserve notices and resolve that discrepancy before reuse.
The inspected `dreamiverse-transformer` tree has no license file. Do not treat published research as permission to copy arbitrary material.
The fetched repository revisions were `ad17816cac4768e60721ce51e2f6adf1bd26d316` for `dreams-api`
and `aa91e07c7770005378fc761d5a3b6f57c36039af` for `dreamiverse-transformer`.

### Native services actually documented

The published path pattern is:

```text
https://prod.puprod.indreams.me/puserver.v1.<Service>/<Method>
```

The page lists approximately 100 method paths across these nine families.
An independent manual recount gives 98 entries, including 55 in DreamService. This is not proof of a complete route inventory.

| Family | Representative methods | Likely responsibility, inferred from names |
|---|---|---|
| IdentityService | LoginUser, GetPlayerIdentity, RefreshUser | Login, profile, session/account state |
| DiscoveryService | SearchCreations, SearchUsers | Search and browsing |
| DreamService | GetSceneVersion, GetDreamMap, PublishScene, GetEditRightRef | Creation metadata, versions, publishing, permissions |
| BlobsService | CreateBlob, FindMissingBlobs | Binary asset transfer coordination |
| SocialService | ListLiked, ListFollowing, ListPlayLater | Social lists and saved queues |
| FeedsService | ListNotifications, ShareCreation | Notifications and feeds |
| ScoreboardsService | PostScoreboardScore, ListScoreboardScores | Persistent scores and ranking |
| CommunityService | GetHighlightPage, ListCommunityJams | Curation and historical community content |
| ImportService | SubscribeImport | Import workflow coordination |

**The route naming resembles gRPC, but it does not prove gRPC, protobuf, HTTP/2, or any particular framing.**
No usable native request/response captures or `.proto` files were found in the sources examined.
The route-list page does not identify its source game version. Its dated progress note gives no year.

The transformer notes separately name `cdn-prod.puprod.indreams.me`.
The older website wiki describes `cdn.indreams.me` image URLs.
Redirecting the RPC hostname alone is therefore not a complete asset-transfer solution.
Returned asset URLs, redirects, authentication, integrity checks, and download authorization need tracing.
Do not invent a blob download URL from these hostname lists.

Community posts report build/auth rejection and a PSN-code-to-LoginUser flow [S12].
These are useful leads, not authenticated captures or a proven session schema.
The static ArchivingDreams page itself reports unsuccessful access.
Neither source establishes current official-server availability, which this investigation did not test.

## 2. The version boundary matters immediately

| Version | Official date and behavior |
|---|---|
| **2.52**, current local game | 7 December 2022, before the server migration [S6] |
| **2.57** | 22 May 2023, migration to a new server and major behavioral changes [S7] |
| **2.64**, planned upgrade | 13 September 2023, final major feature update, audio importer restored [S8] |

The v2.57 release notes explicitly require the update to reconnect to official servers.
They change publishing, deletion, permissions, search, collaboration, scoreboards, and website integration.
**Do not assume the documented `puserver.v1` routes match the 2.52 executable.**
The actual protocol in either local executable remains unverified by this research.

Proposed baseline: target **2.64 first**, after obtaining and verifying the user's own update.
Treat 2.52 compatibility as a separate adapter until evidence proves shared protocol and behavior.
Restoring pre-migration features is a different product from matching the post-migration official service.
A server cannot restore removed client UI or functionality without additional client changes.

## 3. What the port currently provides

Observed source baseline: root `33acf7f`, port `e9b1d88`, renderer `7d25345b`, with existing uncommitted changes.
Source line numbers refer to the inspected working tree and can move.

| Source | Current behavior and implication |
|---|---|
| `port/src/runtime_services.c:293-318` | NetCtl reports disconnected. HTTP object creation returns handles, but transfers fail. HTTP waits return no events |
| `port/src/runtime_services.c:320-330` and export table | NP reports signed out. Several request/callback functions discard state or immediately report signed-out results |
| `port/src/runtime_dreams.c:145-179` | Additional offline account behavior, including not-signed-up state and an offline account identifier |
| `port/src/runtime_dreams_services.c:233-249` | Reachability callback registration stores a callback but never delivers online transitions |
| `port/src/runtime_services.c:694-697` | Tries `runtime_net_resolve()` before its offline exports |
| `port/src/runtime_net.c:27-42,71-127` | `BB_ONLINE` gates an optional `dlopen` module. Missing symbols can fall back to offline behavior |
| `port/gpu/bbnet20/bbnet.h` | Existing module ABI supplies title metadata and guest-thread helpers |
| `port/gpu/bbnet20/bbnet_glue.cpp:615-681` | Registers inherited network/HTTP/NP implementations or the separate Hunter's Dream backend |
| `port/gpu/bbnet20/thd/thd_session.cpp:65-93` | Existing configurable custom-service origin pattern for a different game |
| `tools/agent-run.sh:39-44` | Agent tests use a separate network namespace via `--unshare-net` |

**Changing DNS or replacing a URL is insufficient.** The default client compatibility layer prevents online behavior before any useful service exchange.
There is reusable networking code and a module seam, so implementation need not start with an entirely new network stack.
However, the inherited module is not a validated Dreams solution. It includes unrelated backend defaults and game-specific hooks.
Do not enable `BB_ONLINE` as an exploratory shortcut against public services.

A future implementation needs consistent identities, request lifetimes, errors, cancellation, partial reads, timeouts, and callback/epoll delivery.
Match the relevant PS4 delivery/thread semantics. Do not invent a callback thread or return success for every import.
NPWebApi imports do not prove that Dreams' native content RPCs use NPWebApi rather than HTTP directly.
Trace the exercised paths before implementing a generic PSN replacement.

Local tooling already models some required relationships: `port/tools/dreams_content_inventory.py:52-67`
reads head versions and pinned child references. `port/tools/dreams_content_closure.py:29-36,122-185,201-220`
tracks version pairs, remix ancestry, scene/audio blobs, and special asset hashes.
Its fixtures can seed tests, but they are **not a native network schema**.
Its documented limits exclude opaque payload dependencies and `bloblist.dat` parsing.

### Why the native port can help research

Proposed: instrument the relevant host HTTP/service boundary before TLS, without changing normal behavior.
If the game uses that boundary, it can expose request buffers without breaking HTTPS or obtaining a server private key.
This remains conditional on the actual transport path and any application-level encoding or encryption.
Outgoing requests alone do not reveal valid login-response fields.
Client deserializers, public schemas, or authorized reference traces must supply the missing response contract.
No such tracing was implemented or validated here.

## 4. Define "exactly the same" before promising it

| Target | Assessment |
|---|---|
| Same engine, physics, logic, sculpts, and scene data | Preserve the native engine and exact payload bytes. A server does not replace local simulation |
| Same browsing/download/publish/editor workflows | Plausible, but requires wire compatibility and stateful behavioral tests |
| Same creation/version identities and pinned references | Required. A collection or Dream is not a loose directory of downloadable files |
| Same permission, attribution, collaborator, and deletion rules | Required for editor parity, not optional social polish |
| Original catalog, user accounts, scores, private creations, social graph | Not recoverable merely by implementing the protocol. Requires authorized data and identity migration |
| Identical recommendations and curated rows | Cannot promise identical results without the original algorithms, data, and editorial decisions |
| Actual Sony PSN login, trophies, system friends | Separate platform integration, not automatically provided by a Dreams server |
| Online simultaneous multiplayer | Not an official shipped feature to recreate. The official FAQ says planned multiplayer did not ship [S9] |

A custom server can issue its own authenticated sessions. It does not need Sony credentials to authenticate its own users.
That does **not** mean accepting arbitrary tokens or trusting a claimed owner name.
Separate local accounts from PSN identities and enforce ownership for every privileged operation.

### Examples that make editor parity difficult

The official permissions/deletion guides are concrete behavioral specifications [S10][S11]:
- PUBLIC, PLAYABLE, PRIVATE, listed/unlisted, and moderated states are not interchangeable.
- Owners, collaborators, and strangers have different rights. PLAYABLE does not mean remixable.
- Previously acquired rights to stamped content can survive deletion or collaborator removal.
- A stamped element can survive deletion inside a creation, while deleting a referenced scene can invalidate a Dream.
- Credits persist even when genealogy hides deleted/private content.
- Lead and Latest versions differ. Replacing every reference with the newest version changes behavior.
- Uploading related local versions must preserve permission and genealogy relationships [S13].

The v2.57 baseline also changes observable UI results: Top-100 scoreboards, paginated comments,
recent-history search filters, Play Later retention, local-only homespaces, and removed live website tools.
Returning plausible JSON or an empty success response does not reproduce these behaviors.

## 5. Proposed implementation architecture

```text
Original Dreams guest engine and online UI
    -> opt-in PS4 identity / HTTP compatibility adapter
    -> explicit local/community service origin
    -> version-specific wire adapters
    -> shared creation/permission/version domain model
       + transactional metadata database
       + immutable binary object storage
       + search index and curation
       + social / score / notification services
```

Start with one service process, not nine microservices just because nine API families exist.
A relational database plus a blob directory is sufficient for an initial private instance.
Use an object store and derived search index when scale requires them.
Keep protocol codecs separate from domain rules so version adapters do not duplicate permission logic.

Preserve blobs as opaque bytes first. Full sculpt decompilation or conversion to meshes is **not** required for compatible storage and transfer.
The historical wiki describes LZ4 streams and an `mmmM` header [S14]. It does not define every asset/version format.
Keep client identifiers and dependency references. Use an independent storage digest for integrity without guessing the game's hash algorithm.
Retain reference-aware data and tombstones. Do not garbage-collect every blob associated with a deleted creation.

Default to no network. An opt-in origin must fail closed and must not forward unknown URLs, redirects, or tokens to production hosts.
Keep PS4-compatible TLS policy scoped to the selected custom origin. Do not globally disable certificate verification.
A local fixture server must run in the same private network namespace, or use an explicitly designed local transport bridge.
Keep the agent sandbox and single-GPU lease. Do not remove `--unshare-net` to make tests convenient.

### Public-service obligations

A public deployment also needs access control, rate limits, upload limits, moderation, reports, backups, and recovery tests.
Treat compressed blobs, images, metadata, and imported audio as hostile inputs.
Bound decompression, request sizes, dependency depth, storage usage, and parser execution.
Defend returned-URL handling against SSRF, file access, credential leakage, and redirects to unapproved origins.
The native engine can contain parser vulnerabilities. Server validation cannot prove arbitrary content safe for direct host execution.
Preserve a client sandbox and avoid granting downloaded content unnecessary host access.
Public availability of a creation is not a blanket redistribution license. Resolve creator consent and service terms before mirroring catalogs [S15].
Hosting costs depend on stored unique bytes, replicas/backups, and download traffic, not just account count.
For scale context, 100 accounts each filling a 5 GB quota means 500 GB logical content before deduplication and backups.
The 2024 curation update also changed discovery presentation without a new engine baseline [S16].

## 6. Feasibility gates before a full build

1. **Choose and identify one target build.** Record title/version and the actual service paths it uses.
2. **Inventory the protocol.** Obtain schemas or trace a minimal sanitized request/response exchange.
   Identify framing, response parsing, session fields, errors, and asset URL provenance.
3. **Local login gate.** A real local account/session reaches the intended online UI with no production egress.
   Invalid credentials and expired sessions must also produce correct failure behavior.
4. **Read-only creation gate.** Browse and download one authorized scene with every dependency and pinned version.
   Repeat after clearing caches. Verify actual playable-scene entry, not only a launch log or cover page.
5. **Editor round-trip gate.** Create, save privately, restart, download, edit, publish, and reload without data loss.
6. **Two-account permission gate.** Exercise collaboration, remixing, private stamps, deletion, credits, and blocked release.
7. **Operational gate.** Interrupt uploads and responses, retry, restore a backup, and verify consistent metadata/blob state.

Use deterministic local fixtures, schema/codec tests, and behavior comparisons against an identified original-PS4 baseline where available.
Redact credentials from fixtures. Keep any owned captures/data private unless redistribution is explicitly permitted.
Run scene/editor regressions at the existing 30 Hz simulation cadence. Track passed, failed, blocked, and unverified separately.

A two-to-four-week feasibility timebox is proposed, not a promise that schema recovery will succeed.
If login-response or asset-transfer contracts remain unknown, report that blocker before funding the complete backend.

## 7. How hard: reviewed engineering scenarios

**Difficulty judgment: high, roughly 8/10 for a broad compatible service.** A bounded private subset is materially easier.
These are low-confidence scenario estimates, not benchmarks, measured delivery times, or commitments.
They assume one target generation, available reference evidence, and experienced engineering with AI assistance.
One engineer-month means four engineer-weeks in this table. Failed protocol recovery can block every scenario.

| Workstream | Optimistic / central / pessimistic engineer-weeks |
|---|---:|
| Baseline/protocol evidence plus runtime and identity integration | 4 / 7 / 13 |
| Blob/metadata storage and private transfer | 4 / 8 / 14 |
| Pinned dependencies, versions, release permissions, credits, deletion | 5 / 9 / 16 |
| Collaboration | 3 / 6 / 11 |
| Discovery and social features | 5 / 11 / 19 |
| Scoreboards | 1.5 / 3 / 6 |
| Administration, operations, security, hardening | 6 / 12 / 20 |
| Authorized content-import tooling | 3 / 8 / 16 |
| **Broad core total** | **31.5 / 64 / 115** |

A smaller **owner-only private online save/download slice** is estimated at **8 / 15 / 28 engineer-weeks**.
That is approximately **2 / 4 / 7 engineer-months**, not full editor or collaboration parity.
The broad service is approximately **8 / 16 / 29 engineer-months**.
At two full-time experienced engineers, arithmetic lower bounds are roughly 4 / 8 / 14 calendar months.
Serial protocol discovery, integration, review, and reference access increase actual calendar time.
Three AI workers are not three full-time engineers. They reduce coding/test-writing cost, not unknown-contract risk.
These totals exclude historical catalog acquisition, exact recommendation parity, a full companion website/audio importer,
dual-generation compatibility, unrelated renderer/editor fixes, and indefinite public-service maintenance.
They overlap the minimum slice: do not add the minimum estimate again to the broad total.

**Recommendation:** fund the two-to-four-week evidence spike first, targeting 2.64 and creator-owned content.
Proceed to private online round-tripping only after the login and blob contracts pass their gates.
Re-estimate from working client evidence before committing to the broad server.

## Verification and next resume point

All three research lanes used `rightnow/deepseek-v4-1-flash-1017`, with at most three concurrent workers.
One source-lane attempt hit its turn limit. Its bounded retry delivered a verified report.
The supervisor checked primary sources and requested correction passes for unsupported protocol claims and effort arithmetic.
Only the reviewed conclusions appear here. No server/client integration, gameplay, or official parity test passed in this research.
Verification passed: preflight, 10 doc-lint tests, 21-document lint, report host-neutral/whitespace checks, and both repository diff checks.
These checks cover documentation and repository hygiene, not network compatibility.
Next step requires a separately scoped protocol-evidence task, not silently enabling the existing online module.

Verify report presence and documentation hygiene from `$REPO`:
`test -s notes/dreams-custom-server-research.md && python3 -I port/tools/doc_lint.py && git diff --check && git -C port diff --check`.

## Sources

[S1]: https://archivingdreams.mgik.dev/
[S2]: https://github.com/jaames/dreams-api
[S3]: https://github.com/jaames/dreams-api/wiki/indreams.me-API
[S4]: https://github.com/x1nixmzeng/dreamiverse-transformer
[S5]: https://github.com/gamedev44/Dreams-Exportation/blob/master/src/blobexporter.ts
[S6]: https://docs.indreams.me/en-US/whats-happening/updates/release-notes/dreams/v252
[S7]: https://docs.indreams.me/en-US/whats-happening/updates/release-notes/dreams/v257
[S8]: https://docs.indreams.me/en-US/whats-happening/updates/release-notes/dreams/v264
[S9]: https://docs.indreams.me/en/whats-happening/news/dreams-support-update
[S10]: https://docs.indreams.me/en-US/create/releasing/understanding-permissions
[S11]: https://docs.indreams.me/en-US/create/releasing/understanding-deletion
[S12]: https://www.reddit.com/r/PortingDreams/comments/1x5gt3s/october_13th_for_real_this_time_a_small_update/
[S13]: https://docs.indreams.me/en-US/create/releasing/understanding-related-versions
[S14]: https://github.com/jaames/dreams-api/wiki/Dream-Blob-Data
[S15]: https://docs.indreams.me/en-US/game-info/legal/dreams-content-usage-terms
[S16]: https://docs.indreams.me/en/whats-happening/news/curation2024
