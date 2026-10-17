# DG-LAB transport options — extracted from the official `dglab-bluetooth-protocol` repo

**Scope.** Everything below is extracted from the official protocol repository,
[`dungeonlab-open/dglab-bluetooth-protocol`](https://github.com/dungeonlab-open/dglab-bluetooth-protocol).
The exact files this analysis was performed on are vendored byte-for-byte under
`re/reference/dglab-bluetooth-protocol/` (with upstream commit hash and sha256 sums recorded in the
`PROVENANCE.md` next to them), so every `path#Lnnn` citation below can be resolved offline.
No web search was used for the protocol facts themselves. Where a fact is not in the upstream files,
it says so.

**Evidence tags used throughout:**

| Tag | Meaning |
|---|---|
| `[L]` | Verbatim / directly stated in a local file (path given) |
| `[DER]` | Derived by me from local file content, and verified against the byte dumps in those files |
| `[CONFLICT]` | English and Chinese docs disagree, or a file contradicts itself. Both sides reported |
| `[GAP]` | Not determinable from the local files |

**File inventory.** The repo (22 files total) contains only Markdown protocol descriptions and two `.webp`
images — **no source code, no sample programs, no schema files, no network protocol documents**. Under `coyote/`
there are exactly 10 files:

```
coyote/README.md            coyote/README_CN.md
coyote/v2/README.md         coyote/v2/README_CN.md      coyote/v2/example.md   coyote/v2/example_cn.md
coyote/v3/README.md         coyote/v3/README_CN.md      coyote/v3/example.md   coyote/v3/example_cn.md
```

Sibling device docs that also exist (not Coyote): `civet-edging-sensor/`, `opossum-vibrate-controller/`,
`paw-prints/`, `paw-prints/extvoltageinput/`.

**Changelog note the task asked about.** `README.md#L36-L48` states `2026/10/16: Added English documentation.`
and `2026/10/15: Corrected errors in the Civet, Coyote, and Pawprint protocol documentation and examples.`
The changelog does **not** enumerate which errors were corrected, and the corrections have **not** made the
Coyote docs self-consistent: this extraction found an unresolved A/B pulse-data swap in V2 and a wrong B0
field order in the Chinese V3 doc (both below). Treat "corrected" as "was edited", not "is now verified".

---

## 1. Which transports actually exist

| # | Transport | Documented in this repo? | What it needs |
|---|---|---|---|
| 1 | **BLE GATT direct** (your PC is the BLE central, no phone) | **Yes** — fully. V2 at `coyote/v2/README.md#L5-L17`, V3 at `coyote/v3/README.md#L5-L19` | A BLE 4.0+ adapter and a GATT client. Device must be free (not held by the phone app). |
| 2 | **WebSocket / "socket" / relay / terminal / app protocol** | **No — not in this repo.** It **is** officially documented, in a *different* repo (see the correction below). | A running WebSocket server, the phone running the official APP, the APP scannable via QR code. |

**No WebSocket transport is documented in this repo.** This is a negative result obtained by searching
every Markdown file in the repository (all 20) for `websocket`, `ws://`, `wss://`, `socket`, `relay`,
`server`, `terminal`, `qr`, `http`, `tcp`, `udp`, and port numbers. The **only** matches in the entire
repository are two hyperlinks to the vendor's own website:

- `README.md#L15` — `[contact us](https://www.dungeon-lab.com)`
- `README_CN.md#L15` — `[联系我们](https://www.dungeon-lab.com)`

Consequently the following are all **absent** from the local files, and a third-party app would find
nothing here to connect to:

- No URL scheme, no hostname, no port number, no path, no query parameter.
- No relay/collaboration-server concept, no room/session/target/client ID fields.
- No QR-code pairing flow, no pairing payload format, no app-name string to advertise.
- No text-vs-binary framing rule, no JSON field names, no heartbeat/ping interval.
- No mention that a phone app must be involved at all (`[GAP]` — the docs never describe the official app's
  role beyond calling its intensity display "the number inside the ring in the app", `coyote/v2/README.md#L33`).

The only "protocol" between the docs and a host application is **the BLE GATT byte protocol itself**,
which is transport-agnostic in the sense that anyone may implement the central role. `README.md#L13`:
*"This repository shares the Bluetooth protocols for selected DG-LAB devices. Use the language, framework,
or approach of your choice to integrate these devices into your own projects."* And `README.md#L15`:
*"Commercial use of this content requires prior authorization."*

> **Consequence for the pluggable-transport design:** the only transport that can be implemented from this
> repository is BLE-direct. A WebSocket transport would have to be designed by you (or sourced elsewhere);
> this repo supplies no wire format for it. Architect the transport interface around the BLE role, and treat
> any relay/phone path as an additional implementation you must specify yourself.

### 1.1 Correction (added later): the SOCKET path *is* official — just documented elsewhere

Everything above is still a true statement **about this repository**, but the conclusion "no WebSocket
transport exists" was wrong. The vendor publishes two further, separate repositories for it:

- [`dungeonlab-open/dglab-websocket-simple`](https://github.com/dungeonlab-open/dglab-websocket-simple) —
  the SOCKET protocol spec itself, under `socket/`. **This repo is archived** (upstream's default branch
  is still `main`; the spec is frozen at `socket/v2/README.md`). It also carries the vendor's QA file,
  which is where the decisive quote below comes from.
- [`dungeonlab-open/dglab-websocket-server`](https://github.com/dungeonlab-open/dglab-websocket-server) —
  the reference relay server (not archived).

> An earlier revision of this section cited `DG-LAB-OPENSOURCE/DG-LAB-OPENSOURCE`, `socket/` directory.
> That repository name does not exist: the `dungeonlab-open` organisation contains exactly six repos
> (`dglab-bluetooth-protocol`, `dglab-kit`, `dglab-kit-python`, `dglab-websocket-simple`,
> `dglab-websocket-server`, `.github`) and no `DG-LAB-OPENSOURCE`. The protocol facts below were
> re-checked against `dglab-websocket-simple` and hold; only the citation was wrong.

- It speaks JSON over WebSocket, `{"type","clientId","targetId","message"}`; `type` ∈
  `heartbeat|bind|msg|break|error`; max message length **1950 chars**.
- Pairing is by QR code whose payload is
  `https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#<ws-url>/<clientId>` — the APP must scan it.
- Commands: `strength-{ch}+{mode}+{value}` (mode 0=decrease/1=increase/2=set-to, value 0–200),
  `pulse-{A|B}:["<16 hex chars>",...]` (array ≤100 entries = 10 s), `clear-{1|2}`.
- Upstream reports: `strength-{A}+{B}+{A_limit}+{B_limit}`, `feedback-{0..9}` (5 buttons per channel).

The single most important sentences are in that repo's QA file
(`socket/QA/Websocket_open_source_QA_English.txt`, questions 1 and 29), and they settle the
architecture question:

> *"the Coyote V3 can only connect to the mobile APP via Bluetooth. ... waveforms cannot be sent via
> methods other than the APP (even with remote control)."*

i.e. the SOCKET path does **not** replace BLE — it is an *app-mediated* path. The APP remains the BLE
central and the owner of the intensity ceiling; the third-party program just hands the APP queued data.
Consequences for us (racing FFB, streaming real-time signal):

1. It adds a mandatory phone + WS server (LAN or public) to the loop, plus network RTT, on top of the
   device's own 100 ms frame quantum. For real-time streaming that is strictly worse than BLE-direct.
2. Its model is *queued batches* (`pulse-A:[...]` up to 10 s per send, 50 s queue), i.e. designed for
   **playlists**, not for a signal that changes every frame. `clear-{ch}` before re-sending is the
   documented way to interrupt, which means an interrupt-then-refill cycle per update.
3. It *does* have one genuine advantage: the phone's native intensity dial and limit remain available to
   the user, and `feedback-{index}` gives 10 hardware buttons we could bind to game functions.

**Decision: stay on BLE-direct** (`[link] mode=ble`). This is what is already implemented.

### 1.2 Update (added later): the SOCKET protocol has moved on to V4

§1.1 describes **V3** of the SOCKET protocol, and its conclusions are scoped to V3. Since then the vendor
shipped **V4**, and the V3 spec repository (`dglab-websocket-simple`) has been archived. The live, maintained
counterparts are:

- [`dungeonlab-open/dglab-websocket-server`](https://github.com/dungeonlab-open/dglab-websocket-server) —
  reference relay, now containing **two** servers side by side: `v4-server.ts` (default port `9998`, V4,
  "the recommended protocol") and `v3-server.ts` (port `9999`, kept as legacy). Both are Bun/TypeScript.
- [`dungeonlab-open/dglab-kit`](https://github.com/dungeonlab-open/dglab-kit) — the TypeScript SDK that
  actually generates device commands (`DglabSocket`, `sendPulse`, `requestDevices`, `device.op`, …).

The V4 wire format (from the server README's "V4 protocol" section and dglab-kit's "V4 Protocol Reference"):

- **Topology changed from 1:1 to 1:N.** A controller (our side) may serve *multiple* app clients at once,
  instead of the V3 one-controller-one-app pairing.
- **Connection flow.** The controller connects to `ws://host:9998` with no query string and receives
  `{"type":"hello","clientId":"<controller-client-id>"}`. An app client then connects to
  `ws://host:9998?tid=<controller-client-id>`; the controller receives
  `{"type":"client_attached","clientId":"<app-client-id>"}` and stores that id. Disconnects arrive as
  `client_disconnected` / `controller_disconnected`.
- **Envelope.** `{"type":"message","clientId":"<app-client-id>","data":{…}}` to a specific app. The server
  handles only `type`, `clientId` and `data`; it **does not parse DG-LAB device commands at all** — that is
  the SDK's job.
- **Payload inside `data`.** A `t` field discriminates `req` / `resp` / `ev`. RPC requests carry
  `{t:"req", reqId, m, data}` and are answered with `{t:"resp", reqId, result|error}`. The methods that
  matter to us: `devices.get`, `ping`, `device.op` (submit a `DeviceOperate` task) and `device.op.clear`
  (`ClearOperate`).
- **Devices are addressed, not implicit.** After attaching, the app immediately sends `devices.snapshot`;
  descriptors are `{slotId, name, type:"COYOTE_030"|…, props, slotState}`. Every operation names a `slotId`
  and a channel (`V4Channel.A` / `V4Channel.B`).
- **Intent is task-based, not byte-based.** There is no B0/BF here: you submit *intentions*
  (`addIntensity`, `resetIntensity`, `setTempIntensity(…, duration)`, `sendPulse(…, duration, frames)`,
  `clearOperate`) and they run as timed tasks with `priority 0|1|2` and an `immediate` flag. The app decides
  how to realise them over BLE.
- **Pairing QR.** `https://dungeon-lab.cn/s/?v=1&action=socket&url=<percent-encoded ws URL incl. ?tid=>`
  (different from the V3 `app-download.php#DGLAB-SOCKET#<url>/<clientId>` form).
- **Lifecycle.** Heartbeat every 30 s (no reply needed); close codes `4000` controller gone / `4001` unknown
  controller / `4002` controller idle-timeout (300 s with no app attached). The public endpoint is
  `wss://trex.dungeon-lab.cn/v4`.

What this does and does not change for this project:

1. **The architectural conclusion of §1.1 still holds.** V4 changes the controller↔server↔app vocabulary, not
   the hardware topology: the relay server has no Bluetooth stack, so the **phone app is still the BLE
   central** and still owns the device. There is no server-side API that reaches the Coyote without an app.
2. **The "queued playlist" criticism weakens.** V3's `pulse-A:[…]` batch model is replaced by timed tasks
   with priorities and an `immediate` flag, which is a better fit than V3 for continuously changing input.
   The extra network hop and the app's own scheduling latency remain.
3. **BLE-direct remains the default** (`[link] mode=ble`). V4 is the protocol to implement *if* a socket
   output mode is ever added — not a reason to switch today. `[link] mode=websocket` is therefore still
   unimplemented, and the `[websocket]` config keys are leftovers from an earlier "PC as server" design
   (see the comments in `src/config.hpp`).

> Citation note: the V3 facts in §1.1 are unchanged and still correct for V3. This section, like §1.1, is
> compiled from the vendor's own repositories (server `README.md`; dglab-kit `README.md`, section
> "V4 Protocol Reference") rather than from third-party write-ups. No byte-level capture was performed
> against a live V4 relay, so the frame examples above are quoted, not observed.

---

## 2. BLE (direct) transport

### 2.1 V3 — Coyote 3.0

Service/characteristic table, `coyote/v3/README.md#L7-L11` (identical to `coyote/v3/README_CN.md#L7-L11`):

| Service UUID | Characteristic UUID | Properties | Name | Size | Description |
|---|---|---|---|---|---|
| `0x180C` | `0x150A` | Write | `WRITE` | Up to 20 bytes | All commands are written here |
| `0x180C` | `0x150B` | Notify | `NOTIFY` | Up to 20 bytes | All response messages are returned here |
| `0x180A` | `0x1500` | Read / Notify | `READ/NOTIFY` | 1 byte | Battery level |

**Base-UUID expansion rule**, `coyote/v3/README.md#L13` verbatim:

> Base UUID: `0000xxxx-0000-1000-8000-00805f9b34fb`. Replace `xxxx` with the service or characteristic UUID.

Expanded (`[DER]` — trivial substitution of the four hex digits):

| Short | Full 128-bit UUID |
|---|---|
| `0x180C` | `0000180c-0000-1000-8000-00805f9b34fb` |
| `0x150A` | `0000150a-0000-1000-8000-00805f9b34fb` |
| `0x150B` | `0000150b-0000-1000-8000-00805f9b34fb` |
| `0x180A` | `0000180a-0000-1000-8000-00805f9b34fb` |
| `0x1500` | `00001500-0000-1000-8000-00805f9b34fb` |

Note this is the Bluetooth SIG base UUID, so these 16-bit values are **not** vendor-assigned ranges; `0x180A`
and `0x180C` collide with standard assigned services (Device Information / Pulse Oximeter), and `0x1500`/`0x150A`
/`0x150B` are unassigned. Match on the full 128-bit UUID, and match the **device name** as well.

**BLE advertised names**, `coyote/v3/README.md#L15-L19` verbatim:

> Stimulator 3.0: `47L121000`
> Wireless sensor: `47L120100`

`[GAP]` The docs do not say whether these are complete advertised names or name prefixes, whether they appear
in the advertising payload or only in the GAP `Device Name` characteristic, or whether other Coyote SKUs use
different names. There is no name for a "V3" distinct from "3.0" — the name string contains no version field,
unlike V2. `47L120100` is the **wireless sensor** accessory, not the stimulator.

### 2.2 V2 — Coyote 2.0

Table, `coyote/v2/README.md#L5-L10` (identical to `coyote/v2/README_CN.md#L5-L10`):

| Service UUID | Characteristic UUID | Properties | Name | Size |
|---|---|---|---|---|
| `0x180A` | `0x1500` | Read / Notify | `Battery_Level` | 1 byte |
| `0x180B` | `0x1504` | Read / Write / Notify | `PWM_AB2` | 3 bytes |
| `0x180B` | `0x1505` | Read / Write | `PWM_A34` | 3 bytes |
| `0x180B` | `0x1506` | Read / Write | `PWM_B34` | 3 bytes |

**Base UUID**, `coyote/v2/README.md#L23` verbatim:

> Base UUID: `955Axxxx-0FE2-F5AA-A094-84B8D4F3E8AD`. Replace `xxxx` with the service UUID.

`[CONFLICT]` — this sentence says to substitute into the **service** UUID only, but the table also needs
characteristic UUIDs expanded. The Chinese line (`coyote/v2/README_CN.md#L23`) says exactly the same
("将 xxxx 替换为服务的 UUID"). `[DER]` by analogy with V3 both should be expanded:
`955a180a-…`, `955a180b-…`, `955a1500-…`, `955a1504-…`, `955a1505-…`, `955a1506-…` (suffix
`-0fe2-f5aa-a094-84b8d4f3e8ad`).

**BLE advertised name**, `coyote/v2/README.md#L21` verbatim: Stimulator 2.0: `D-LAB ESTIM01`

### 2.3 Write / notify / read, payload, and MTU — what is and is not stated

- **Write:** all commands go to V3 `0x180C/0x150A`, and the table's Properties field says `Write`.
  The B0 command is exactly 20 bytes and the BF command exactly 7 bytes (`coyote/v3/README.md#L31`, `#L115`).
  The maximum stated payload on that characteristic is **20 bytes** (`#L9`).
- **Notify:** all responses come back on `0x180C/0x150B` (`#L137`): *"All stimulator data is returned through
  notifications on `0x180C -> 0x150B`. Subscribe to this characteristic after connecting."* Maximum stated
  payload 20 bytes.
- **Read:** only the battery characteristic `0x180A/0x1500` is readable, 1 byte.
- **Write-without-response:** **not specified** (`[GAP]`). The V3 property column says only `Write`; the B0
  text says *"Send this 20-byte command every 100 Ms"* (`#L31`) but attaches no request/response requirement
  to the write itself. Nothing in the repository mentions the ATT write *type*.
- **MTU:** **not mentioned anywhere** (`[GAP]`). The docs never use the word MTU, never request MTU
  negotiation, and never discuss fragmentation or long writes. `[DER]` The stated 20-byte maximum is exactly
  the ATT_MTU-minus-3 default payload (23 − 3), which is the strongest available hint that the device is
  intended to work at the **unnegotiated default ATT MTU of 23** and that no MTU exchange is needed. This is
  an inference from the number 20, not a documented statement.
- **Pairing / bonding:** **not mentioned anywhere** (`[GAP]`). No pairing, bonding, passkey, "Just Works",
  or security-requirement text appears in any file. `[DER]` Since all three characteristics are
  Write/Notify/Read with no encryption requirement stated, the reasonable starting assumption is that an
  unpaired connection works, but this is **not documented** and must be confirmed on hardware.
- **Connection interval / throughput:** not discussed (`[GAP]`). The only timing statement is the 100 ms
  command cadence.

---

## 3. V3 command codec — authoritative byte tables

Header `coyote/v3/README.md#L25-L27`: *"Unlike V2, this protocol does not require byte-order conversion."*
(Chinese, `coyote/v3/README_CN.md#L27`: *"与 V2 协议不同的是，数据无需进行大小端转换"* — same meaning.)
The B0 byte order was also **verified byte-by-byte against all 16 hex dumps** in
`coyote/v3/README.md#L158-L198` `[DER]`; see §6.1 for the one documentation error this exposed.

### 3.1 `0xB0` — channel intensity + waveform data, 20 bytes, every 100 ms

Layout, `coyote/v3/README.md#L34` verbatim:

> `0xB0` (1-byte command header) + sequence number (4 bits) + intensity interpretation mode (4 bits) + channel A intensity setting (1 byte) + channel B intensity setting (1 byte) + 4 channel A Pulse frequency values (4 bytes) + 4 channel A Pulse intensity values (4 bytes) + 4 channel B Pulse frequency values (4 bytes) + 4 channel B Pulse intensity values (4 bytes)

Byte-offset table (`[DER]`, verified against the repo's own hex dumps):

| Offset | Bits | Field | Valid range | Out-of-range behaviour |
|---|---|---|---|---|
| 0 | 7–0 | Command header `0xB0` | fixed | — |
| 1 | 7–4 | Sequence number (4 bits) | `0b0000`–`0b1111` (0–15) | — |
| 1 | 3–2 | Intensity interpretation mode, channel A (2 bits) | `0b00`/`0b01`/`0b10`/`0b11` | — |
| 1 | 1–0 | Intensity interpretation mode, channel B (2 bits) | `0b00`/`0b01`/`0b10`/`0b11` | — |
| 2 | 7–0 | Channel A intensity setting | 0–200 | treated as 0 (see `[CONFLICT]` in §6.2) |
| 3 | 7–0 | Channel B intensity setting | 0–200 | treated as 0 (see `[CONFLICT]` in §6.2) |
| 4–7 | 7–0 each | Channel A Pulse frequency ×4 | each 10–240 | **any** invalid value → all 4 A pairs discarded |
| 8–11 | 7–0 each | Channel A Pulse intensity ×4 | each 0–100 | **any** invalid value → all 4 A pairs discarded |
| 12–15 | 7–0 each | Channel B Pulse frequency ×4 | each 10–240 | **any** invalid value → all 4 B pairs discarded |
| 16–19 | 7–0 each | Channel B Pulse intensity ×4 | each 0–100 | **any** invalid value → all 4 B pairs discarded |

Total 20 bytes. Timing, `#L82`: *"Every B0 command contains four frequency/intensity pairs per channel.
Each pair represents 25 ms of output, so four pairs cover 100 Ms."*

Note the payload order — **all four frequencies for a channel, then all four intensities for that channel**,
then the same for the other channel. A naive reading of the layout sentence could produce
`{f1,i1,f2,i2,…}`; the hex dumps prove it is `{f1,f2,f3,f4,i1,i2,i3,i4}`.

### 3.2 Sequence number and the B1 acknowledgement handshake

`coyote/v3/README.md#L39` verbatim:

> The sequence number ranges from `0b0000` to `0b1111`. When changing channel intensity, use a nonzero
> sequence number to request a B1 response containing the updated intensities and the same sequence number on
> characteristic `0x150B`. Set it to `0b0000` if no intensity response is needed.

`coyote/v3/README.md#L41` verbatim (the "wait for response" rule):

> When changing intensity with a nonzero sequence number, wait for a B1 response with the matching sequence
> number before making another intensity change.

`coyote/v3/README_CN.md#L39-L40` verbatim (Chinese; note it is worded as a *recommendation*, and adds a rationale):

> 序列号范围 (0b0000 ~ 0b1111), 如果输入的数据中修改了脉冲主机的通道强度，设置序列号 >0, 脉冲主机会通过
> B1 回应消息以相同的序列号将修改后的通道强度从特性 0x150B 返回，如果不需要脉冲主机反馈通道强度，则序列号
> 设置 0b0000 即可。
> 另外，为避免产生问题，当通过 B0 指令修改通道强度且序列号不为 0 时，建议等待 150B 返回 B1 且为相同序列号
> 的信息后，再对通道强度进行修改。

`[CONFLICT]` — **strength of the rule**: the English says flatly *"wait for a B1 response … before making
another intensity change"*; the Chinese says *"建议等待"* ("it is recommended to wait"), with the rationale
*"为避免产生问题"* ("to avoid problems"). Both describe the same handshake; only the normative force differs.
Treat it as mandatory — the Chinese calls it a recommendation to avoid unspecified problems, i.e. the failure
mode of ignoring it is undefined, not benign.

Key derived rules (`[DER]`):

- Only **one** intensity change may be outstanding at a time. Sequence numbers need not increment by one and
  are not a rolling frame counter — they only need to be non-zero and distinct from any still-pending value.
- A B0 that carries **no** intensity change should set sequence number `0b0000` (and, by the reference
  algorithm, interpretation mode `0b0000`).
- The B1 handshake gates **intensity changes only**. Waveform data (`4×freq + 4×int` per channel) keeps
  streaming every 100 ms regardless — that is exactly what the sample code does: it emits `orderNo = 0` and
  mode `0b0000` on every frame while an intensity change is in flight
  (`coyote/v3/README.md#L264-L266`).
- The reference algorithm uses `orderNo += 1` starting from 0, so the first non-zero value is `1`, and — see
  §6.3 — the pseudocode is itself buggy at step 16.

### 3.3 Intensity interpretation mode (the 2-bit encoding)

`coyote/v3/README.md#L45`: *"The upper two bits specify how to interpret channel A's intensity setting; the
lower two bits specify the mode for channel B."* Chinese, `coyote/v3/README_CN.md#L44`: *"强度值解读方式的
4bits 分为两个部分，高两位 bits 代表 A 通道解读方式，低两位 bits 代表 B 通道解读方式"* — **the two docs agree.**

**How the 4-bit nibble packs both channels** — this is the single most error-prone detail. The nibble lives in
byte 1: **the four bits *below* the sequence number are the mode nibble, and the sequence number is the four
bits *above* it.** The mode nibble's *high* two bits are channel A, its *low* two bits are channel B.

```
byte 1 bit:   7   6   5   4  |  3   2  |  1   0
            [ sequence number ] [ A mode ][ B mode ]
              4 bits, 0b0000-0b1111
```

Worked example from the docs (`coyote/v3/README.md#L63`, mode `0b1101`, both channels starting at 10, A
setting 5, B setting 8 → A becomes 5, B becomes 18): mode nibble = `0b1101`, so A bits = `0b11` (absolute →
5) and B bits = `0b01` (increase → 10 + 8 = 18). `[DER]` This also means only **6 of the 16** possible mode
values are exercised in the doc examples, but all 16 are well-defined.

| Bits | Behaviour (`coyote/v3/README.md#L47-L52`) |
|---|---|
| `0b00` | Leave the channel unchanged; ignore its intensity setting. |
| `0b01` | Increase by the setting. For example, 15 adds 15 to the current intensity. |
| `0b10` | Decrease by the setting. For example, 17 subtracts 17 from the current intensity. |
| `0b11` | Set an absolute intensity. For example, 32 sets the channel to 32. |

Combined examples, `coyote/v3/README.md#L56-L63`, both channels starting at intensity 10 — reproduced exactly:

| Mode   | A setting | B setting | Resulting A intensity | Resulting B intensity |
|--------|-----------|-----------|-----------------------|-----------------------|
| `0b0000` | 5 | 8 | 10 | 10 |
| `0b0100` | 5 | 8 | 15 | 10 |
| `0b0010` | 5 | 8 | 10 | 2 |
| `0b0011` | 5 | 8 | 10 | 8 |
| `0b0110` | 5 | 8 | 15 | 2 |
| `0b1101` | 5 | 8 | 5 | 18 |

A second table, `coyote/v3/README.md#L71-L76`, channel A starting at intensity 10, showing the range clamps:

| Mode   | A setting | Resulting A intensity | Notes |
|--------|-----------|-----------------------|-------|
| `0b0100` | 195 | **200** | relative increase saturates at the 0–200 ceiling (10 + 195 = 205 → 200) |
| `0b1000` | 20  | **0**   | relative decrease saturates at 0 (10 − 20 = −10 → 0) |
| `0b0100` | 201 | 10    | setting out of range → treated as 0 → no change |
| `0b1100` | 201 | **0**   | setting out of range → treated as 0 → absolute 0 |

`[DER]` The third row demonstrates the crucial asymmetry: for a relative mode, an out-of-range setting becomes
0, so nothing happens; for the absolute mode `0b11`, an out-of-range setting becomes 0, which **actively zeroes
the channel**. The 4th row is also the documented way to force one channel to 0 (see §3.6 and §7).

### 3.4 `0xBF` — soft limits and balance parameters, 7 bytes

`coyote/v3/README.md#L111-L119`. Warning box, `#L113` verbatim:

> **⚠️ BF takes effect immediately and has no response. Send BF to set the soft limits after every
> reconnection to prevent unexpected soft-limit values.**

> The 7-byte BF command sets the channel intensity soft limits, Pulse frequency balance parameters, and Pulse
> intensity balance parameters.
>
> `0xBF` (1-byte command header) + channel A/B intensity soft limits (2 bytes) + channel A/B Pulse frequency balance parameters (2 bytes) + channel A/B Pulse intensity balance parameters (2 bytes)

Byte-offset table:

| Offset | Field | Valid range | Out-of-range behaviour |
|---|---|---|---|
| 0 | Command header `0xBF` | fixed | — |
| 1 | Channel intensity soft limit (first channel) | 0–200 | existing limit unchanged |
| 2 | Channel intensity soft limit (second channel) | 0–200 | existing limit unchanged |
| 3 | Pulse frequency balance parameter (first channel) | 0–255 | `[GAP]` not stated |
| 4 | Pulse frequency balance parameter (second channel) | 0–255 | `[GAP]` not stated |
| 5 | Pulse intensity balance parameter (first channel) | 0–255 | `[GAP]` not stated |
| 6 | Pulse intensity balance parameter (second channel) | 0–255 | `[GAP]` not stated |

`[GAP]` **Which of offsets 1/2 is channel A is not stated.** The English calls the field "channel A/B intensity
soft limits (2 bytes)" and the Chinese "AB 两通道强度软上限 (2bytes)" — both name the channels in A-then-B
order without ever saying which byte is which. `[DER]` A-then-B is the natural reading and matches the B0
convention (A first), but this is an inference, **not documented**, and must be confirmed on hardware.
The same ambiguity applies to offsets 3/4 and 5/6. There is **no BF response message at all**, so a wrong
order silently swaps the two channels' soft limits.

Semantics, `coyote/v3/README.md#L121-L133`:

- **Channel intensity soft limit** — *"The soft limit caps the maximum channel intensity and persists across
  power cycles. Valid values are 0–200; out-of-range values leave the existing limit unchanged."* And: *"with
  limits of 150 for A and 30 for B, the intensities remain within 0–150 and 0–30 respectively, whether changed
  using a Wheel or a B0 command. Channel intensity cannot exceed its soft limit."*
- **Frequency Balance Parameter 1** (Pulse frequency balance) — 0–255, persists across power cycles.
  *"adjusts the relative sensation of low- and high-frequency Pulses at a fixed channel intensity. Higher values
  produce a stronger throbbing sensation at low frequencies."* This is the parameter that decides the X:Y split
  inside the Pulse frequency value (`coyote/README.md#L31`).
- **Frequency Balance Parameter 2** (Pulse intensity balance) — 0–255, persists across power cycles.
  *"adjusts pulse width and the relative sensation of different frequencies at a fixed channel intensity.
  Higher values make low-frequency Pulses feel stronger."*

`[DER]` Because BF persists but has no readback and no response, the only safe pattern is to write known values
on every connection — you cannot query the current soft limit, and the "unexpected soft-limit values" the
warning mentions are exactly the stale values from a previous session.

### 3.5 `0xB1` — response message (notify on `0x150B`)

`coyote/v3/README.md#L139-L145`. Response rules, `#L141` verbatim:

> When channel intensity changes, the device immediately returns the current intensities in a B1 message. For
> a change caused by B0, the response uses the sequence number from that command; otherwise, the sequence
> number is 0.

> `0xB1` (1-byte command header) + sequence number (1 byte) + actual channel A intensity (1 byte) + actual channel B intensity (1 byte)

| Offset | Field | Range / notes |
|---|---|---|
| 0 | Command header `0xB1` | fixed |
| 1 | Sequence number | **1 full byte here** (not a nibble). Echoes the B0's 4-bit value; `0` when the change was not caused by B0 |
| 2 | Actual channel A intensity | 0–200 |
| 3 | Actual channel B intensity | 0–200 |

`[DER]` Total length is 4 bytes, though the docs do not state a total length. Note the sequence number is a
**4-bit value carried in a full byte** on the response; values above 15 are not meaningful. Also note the
notify path reports **both** channels on every intensity change, and it also fires for changes made by the
physical Wheel with sequence number 0 — so a host must tolerate unsolicited B1 messages with sequence 0.

### 3.6 `0xBE` — deprecated

`coyote/v3/README.md#L147` and `coyote/v3/README_CN.md#L121`, in their entirety:

> #### BE Message (Deprecated)

**That is all that is written.** `[GAP]` No length, no field layout, no semantics, no reason, no replacement,
no version in which it stopped being used. Do not implement it, and do not assume anything about it.

### 3.7 The `inputValue (10..1000) -> transmitted (10..240)` conversion formula

`coyote/v3/README.md#L84-L102`, verbatim as given:

```
Input range: 10–1000
pulseFrequency = when (inputValue) {
    in 10..100 -> {
        inputValue
    }
    in 101..600 -> {
        (inputValue - 100)/5 + 100
    }
    in 601..1000 -> {
        (inputValue - 600)/10 + 200
    }
    else -> {
        10
    }
}
```

The Chinese version (`coyote/v3/README_CN.md#L62-L78`) is the same three branches with identical arithmetic and
an identical `else -> 10` fallback. **The two agree.**

`[DER]`/`[GAP]` Two things the formula does not pin down:

1. **Rounding of the division is unspecified.** The pseudocode is integer-flavoured but `/` in the original
   (Kotlin-style `when`) is integer division only if the operands are integers — the docs never say floor, truncate,
   or round. Note that `(inputValue - 100)/5 + 100` with truncation gives, e.g., `101 → 100` and
   `104 → 100`, so several inputs collapse to the same byte. This matters if you want an exactly linear ramp.
2. **The endpoint arithmetic does not line up perfectly.** With exact division, `600` hits branch 2 and yields
   `(600-100)/5+100 = 200`, while `601` hits branch 3 and yields `(601-600)/10+200 = 200` — continuous, good.
   But `101 → (1)/5+100 = 100` (truncating) and `100 → 100` — also continuous. The branch boundaries are
   therefore consistent under truncation.

Cross-check table reproduced from `coyote/README.md#L50-L60` (identical in `README_CN.md#L44-L54`), showing the
transmitted value is **the output-unit duration in milliseconds, not a frequency**:

| Pulse frequency value | Transmitted value | Electrical pulse frequency |
|:---:|:---:|:---:|
| 10 ms | 10 | 100 Hz |
| 20 ms | 20 | 50 Hz |
| 50 ms | 50 | 20 Hz |
| 100 ms | 100 | 10 Hz |
| 110 ms | 102 | 9 Hz |
| 150 ms | 110 | 6.6 Hz |
| 650 ms | 205 | 1.53 Hz |
| 680 ms | 208 | 1.47 Hz |
| 750 ms | 215 | 1.33 Hz |

`[DER]` **The transmitted value is inverted with respect to perceived frequency:** a *smaller* byte is a
*higher* electrical frequency (10 → 100 Hz, 240 → ~4.2 Hz). This inverts the sign of any "more intensity =
bigger number" intuition and is a classic source of reversed-control bugs.

The doc's own worked example of a linear *electrical frequency* ramp, `coyote/README.md#L35-L39`, so the
compression is visible: choose 1…10 Hz → 1000, 500, 333, 250, 200, 166, 142, 125, 111, 100 ms → apply the
formula → transmitted `240, 180, 146, 130, 120, 113, 108, 105, 102, 100` → *"Send groups of four values every
100 ms, padding an incomplete group with 10."* A linear ramp in the *Pulse frequency value* instead
(`#L41-L44`): 100…1000 → `100, 120, 140, 160, 180, 200, 210, 220, 230, 240`.

### 3.8 Suppressing output on one channel — the documented trick

`coyote/v3/README.md#L82` and `#L109`, verbatim:

> If any value for a channel is out of range, the device discards all four pairs for that channel.

> - To output a Pulse on only one channel, include at least one invalid value in the other channel's data,
>   such as a Pulse intensity above 100.

`[DER]` This is why every single-channel example in the repo writes `{0,0,0,101}` into the idle channel's Pulse
intensity slots — e.g. `coyote/v3/README.md#L158`:
`0xB00000000A0A0A0A000A141E0000000000000065`. The `0x65` = 101 in the last byte is deliberately illegal, which
kills channel B's whole 100 ms block while channel A plays normally. Note this is *not* the same as intensity 0:
the documented zero-intensity method is mode `0b11` with setting out of range or 0 (see §3.3), while this trick
suppresses the **waveform** block.

---

## 4. V2 codec (for reference)

`coyote/v2/README.md#L12-L17`. V2 has no command bytes at all — it is a set of 24-bit bitfield writes.

| Name | Description | Data layout |
|---|---|---|
| `Battery_Level` | Current battery level | 1 byte (integer, 0–100) |
| `PWM_AB2` | Channel A/B intensity | Bits 23–22: reserved; bits 21–11: actual channel A intensity; bits 10–0: actual channel B intensity |
| `PWM_A34` | Channel B Pulse data | Bits 23–20: reserved; bits 19–15: Az; bits 14–5: Ay; bits 4–0: Ax |
| `PWM_B34` | Channel A Pulse data | Bits 23–20: reserved; bits 19–15: Bz; bits 14–5: By; bits 4–0: Bx |

`[CONFLICT]` — see §6.4. The Name column (`PWM_A34`) and the Description column ("Channel B Pulse data") for the
same row name **opposite channels**, and the bit names do not disambiguate (`Ax` under a row described as
channel B). `coyote/v2/README_CN.md#L12-L17` carries the identical contradiction, so it is not a translation
slip. **Unresolved in the local docs.**

Parameter semantics, `coyote/v2/README.md#L27-L71`:

- **S (intensity, in `PWM_AB2`)**: range 0–2047, *"**must not exceed 2047**"*. *"Each intensity step in the
  official app corresponds to an increase of 7: the device value is seven times the displayed value. Writing a
  new S value changes the channel intensity immediately, and that value remains in effect."*
- **X** = bits 4–0 (5 bits), range 0–31. **Y** = bits 14–5 (10 bits), range 0–1023. **Z** = bits 19–15 (5 bits),
  range 0–31.
- **Timing**: X consecutive pulses over X ms, then Y ms idle, repeating.
- **Frequency**: `Frequency = X + Y`, `Actual pulse frequency = Frequency / 1000`, `Frequency` range 10–1000.
  Recommended split (`coyote/v2/README.md#L58-L61`):
  `X = ((Frequency / 1000)^ 0.5) * 15`, `Y = Frequency - X`. *"If X:Y exceeds 1:9 (for example, `[8,2]`), the
  overall sensation becomes weaker."*
- **Pulse width**: *"The actual width is `Z * 5 µs`. For example, `Z = 20` gives `5 * 20 µs = 100 µs`."* And the
  safety-relevant note: *"Pulse widths above 100 µs (`Z > 20`) are more likely to cause a stinging sensation."*
- **Validity**: *"Each `[X,Y,Z]` set is valid for only 0.1 s: after receiving a set, the device outputs the
  corresponding Pulse for 0.1 s and then stops."*
- **Byte order** `[GAP]`: the English doc never mentions it. The V3 doc asserts V3 *"does not require byte-order
  conversion"* **unlike V2** (`coyote/v3/README.md#L27`), which implies V2 *does* require it — but the V2 page
  does not say what the required order is. `[DER]` The `example.md` byte strings disambiguate it:

  ```
  x = 1,y = 9,z = 0,  bytes = 210100     (coyote/v2/example.md#L10)
  ```

  `X=1, Y=9, Z=0` → `(Z<<15)|(Y<<5)|X = 0x000121`. The bytes actually written are `21 01 00`, i.e. the 24-bit
  value **little-endian** (least significant byte first). Same for `x=1,y=18,z=20` → `0x0A0241` → bytes
  `41 02 0A` (`coyote/v2/example.md#L33`). So: compute the 24-bit word, then transmit LSB-first.

---

## 5. V2 vs V3 — comparison and conversion

| Aspect | V2 | V3 |
|---|---|---|
| Advertised name | `D-LAB ESTIM01` | `47L121000` (stimulator), `47L120100` (wireless sensor) |
| Base UUID | `955Axxxx-0FE2-F5AA-A094-84B8D4F3E8AD` | `0000xxxx-0000-1000-8000-00805f9b34fb` |
| Write characteristic | `0x180B/0x1504`, `0x1505`, `0x1506` (3 bytes each) | `0x180C/0x150A` (up to 20 bytes) |
| Notify characteristic | `0x180B/0x1504` (`PWM_AB2`) | `0x180C/0x150B` |
| Battery | `0x180A/0x1500`, 1 byte, 0–100 | `0x180A/0x1500`, 1 byte, 0–100 |
| Command structure | none — raw 24-bit bitfield writes | explicit command bytes `0xB0`, `0xBF`; responses `0xB1` |
| Writes needed per 100 ms | **3** (intensity + A pulse + B pulse, on 3 different characteristics) | **1** (single 20-byte B0 carries both channels) |
| Byte order | little-endian 24-bit word (derived, §4) | none needed |
| Output window | 100 ms | 25 ms per pair × 4 pairs = 100 ms |
| Intensity parameter | `S`, 0–2047, one characteristic `PWM_AB2`, bits 21–11 (A) / 10–0 (B); app steps of 7 | setting byte 0–200, plus persistent **soft limit** 0–200 per channel |
| Waveform frequency | `X` (0–31) + `Y` (0–1023) separately; `Frequency = X+Y`, 10–1000 | single transmitted byte 10–240; X/Y split delegated to Frequency Balance Parameter 1 |
| Pulse width / waveform intensity | `Z`, 0–31, width = `Z × 5 µs`; app display range 0–20 | byte 0–100, unitless; no direct µs mapping given |
| Soft limit | none documented | `0xBF`, persists across power cycles |
| Acknowledge handshake | none documented | sequence number + `B1` response; one intensity change in flight |
| Reserved for accessories | — | `0xBE`**deprecated**; V3 accessory interactions "being revised" (`README.md#L50`) |
| Allowed to exceed stated maxima | S "must not exceed 2047" | intensity setting treated as 0 if out of range; soft limit unchanged if out of range |

### Conversion formulas

`coyote/README.md#L85-L87`, verbatim:

> V3 Pulse frequency: calculate V2 `(X + Y)`, then apply the conversion from `(10–1000)` to `(10–240)`.
>
> V3 Pulse intensity: V2 `(Z * 5)`.

`coyote/README_CN.md#L79-L81` is identical: *"V3 波形频率 = V2 (X + Y) 后，执行 (10 ~ 1000) -> (10 ~ 240) 的转化"*
and *"V3 波形强度 = V2 (Z * 5)"*.

`[DER]` Verified against the docs' own numbers: V2 `Z = 20` → `20 × 5 = 100` = V3's maximum waveform intensity,
and `coyote/README.md#L70` states the equivalence directly: *"The approximate correspondence is: V2 Pulse
intensity 20 ≈ V3 Pulse intensity 100."* Note the V2→V3 intensity map is therefore only faithful for
**Z ≤ 20**; the raw protocol allows Z up to 31 (`31 × 5 = 155`, above V3's 0–100), so a converter must clamp.
The docs do not say what the device does with a V3 waveform intensity above 100 other than "discard the whole
channel block" when it is sent in a B0 (§3.8) — so clamping is mandatory, not optional.

`[DER]` **Channel intensity does not have a documented V2↔V3 conversion.** V2 S is 0–2047 (with app values =
device/7, i.e. app 0–~292) and V3 is 0–200 with a 0–200 soft limit. The docs give no mapping. Do not assume
`S = V3 × 7` or similar; `[GAP]`.

---

## 6. Documentation inconsistencies and errors found

The task asked specifically whether English and Chinese agree and whether the docs are self-consistent after the
2026/10/15 "corrected errors" changelog entry. They are **not** self-consistent. Five findings, ordered by
severity.

### 6.1 The Chinese V3 B0 layout lists the waveform fields in the wrong order `[CONFLICT]`

`coyote/v3/README_CN.md#L34` (and repeated at `#L127`):

```
0xB0 (1byte 指令 HEAD)+ 序列号 (4bits)+ 强度值解读方式 (4bits)+A 通道强度设定值 (1byte)+B 通道强度设定值 (1byte)+
A 通道波形频率 4 条 (4bytes)+A 通道波形强度 4 条 (4bytes)+B 通道波形频率 4 条 (4bytes)+B 通道波形强度 4 条 (4bytes)
```

Read literally this puts **A's four frequencies *and* A's four intensities first, then B's
frequencies *and* B's intensities**, i.e. `[…, A_f0..3, A_i0..3, B_f0..3, B_i0..3]`. The English
(`coyote/v3/README.md#L34`, quoted in §3.1) says the same thing in words but its **"More Examples" data line**
(`#L153`) prints the pairs explicitly — `A 通道波形频率 {x,x,x,x} + A 通道波形强度 {x,x,x,x} + B 通道波形频率
{x,x,x,x} + B 通道波形强度 {x,x,x,x}` — which is the same grouping, and **matches the hex dumps**.

`[DER]` I resolved this by decoding all 16 `HEX:` strings in `coyote/v3/README.md#L158-L198`:

- `B00000000A0A0A0A000A141E0000000000000065` — bytes 0–1 `B0 00`, 2–3 `00 00`, 4–7 `0A 0A 0A 0A` (=A freq
  {10,10,10,10}), 8–11 `00 0A 14 1E` (=A int {0,10,20,30}), 12–15 `00 00 00 00` (B freq), 16–19
  `00 00 00 65` (B int, last = 101 = deliberate invalid).
- `B00000000A0A0A0A000A141E0A0A0A0A00000000` — B freq at 12–15 `0A 0A 0A 0A` = {10,10,10,10}, B int at 16–19
  `00 00 00 00`. The B frequencies are **not** at 8–11, which is where frequencies would sit if the
  groups were interleaved as `f_A,i_A,f_B,i_B`.

**Conclusion: the byte order in §3.1 is correct and the grouping in both docs' prose is correct; the hex
dumps corroborate it. But the grouping is genuinely easy to misread as interleaved, and the *Chinese* line at
`#L127` writes the payload as a flat concatenation without braces, so implementers following the Chinese page
can build a wrong frame.** Treat §3.1 as authoritative.

### 6.2 Out-of-range channel intensity setting: "treated as 0" vs "nothing happens" `[CONFLICT]`

- English, `coyote/v3/README.md#L67`: *"Each setting is one byte, with a valid range of 0–200. **Out-of-range
  settings are treated as 0.**"*
- Chinese, `coyote/v3/README_CN.md#L50`: *"通道强度设定值长度为 1 字节，值有效范围 (0~ 200), **输入范围外的值均以 0
  处理**。"* — literally "values outside the input range are all handled as 0", i.e. **the same as English.**

They agree on the wording. The apparent contradiction is with the **Chinese example table** that follows
(`README_CN.md#L52`, mirrored in English at `#L71-L76`): row 3 says mode `0b0100` (relative increase) with
setting 201 leaves A at 10, i.e. "nothing happens", while row 4 says mode `0b1100` (absolute) with setting 201
sets A to 0. Both are consistent with "treated as 0" *if* you read mode `0b00`/relative-with-0 as a no-op and
absolute-0 as an assignment: relative +0 changes nothing, absolute 0 zeroes the channel. **So there is no
contradiction here — but the two rows look contradictory at a glance and the docs never spell out the
distinction.** `[DER]` The practical rule: *an out-of-range setting is silently reinterpreted as 0; whether that
is a no-op or a channel-zeroing depends on the mode.*

### 6.3 The reference sequence-number pseudocode contradicts its own worked example `[CONFLICT]`

`coyote/v3/README.md#L300-L306` (identical to `coyote/v3/README_CN.md#L276-L280`):

```
15-> (100ms cycle) B0 prepare to write
     BLE WRITE 150A (0xB0 + orderNo (0b0010) + strengthParsingMethod (0b1000) + strengthSettingValueA (1) + ......)
16-> 150B returns channel A intensity
     BLE NOTIFY 150B (0xB1 + returnOrderNo (2) + returnStrengthValueA (5) + ......)
     returned sequence number = 2, returned channel A intensity = 5
     strengthDataCallback (1,5)
```

The write uses sequence number `0b0010` (= 2) and the notify correctly echoes 2, and the prose says
"returned sequence number = 2" — **but the pseudocode calls `strengthDataCallback (1,5)` with `1`.** Since the
callback's whole purpose is `if (returnOrderNo == inputNo)` (`#L240`), this comparison would fail and
`isInputAllowed` would never be restored, deadlocking all further intensity input. Step 18 (`#L310-L313`) has
the same class of inconsistency: it writes `orderNo (0b0001)` but the comment says the callback is
`strengthDataCallback (1,0)` while `strengthZero()` set `orderNo = 1`.

`[DER]` A second latent bug in the same pseudocode: the callback compares against **`inputNo`** (`#L240`),
a variable that is never assigned anywhere — the assignment is to **`inputOrderNo`** (`#L224`, `#L252`). A
literal transcription will not compile. **Do not copy this pseudocode; implement the handshake from the rules
in §3.2.**

### 6.4 V2 `PWM_A34` / `PWM_B34` channel labels are swapped or ambiguous `[CONFLICT]`

`coyote/v2/README.md#L16-L17`:

| Name | Description | Data layout |
|---|---|---|
| `PWM_A34` | **Channel B** Pulse data | Bits 23–20: reserved; bits 19–15: **Az**; bits 14–5: **Ay**; bits 4–0: **Ax** |
| `PWM_B34` | **Channel A** Pulse data | Bits 23–20: reserved; bits 19–15: **Bz**; bits 14–5: **By**; bits 4–0: **Bx** |

The Name says `A`, the Description says channel `B`, and the bit names say `A` again. The Chinese table
(`coyote/v2/README_CN.md#L16-L17`) is identically self-contradictory ("PWM_A34 | B 通道波形数据 | …Az…Ay…Ax").
There is no way to resolve this from the local files: `[DER]` the one thing that *is* consistent is that the
`x`/`y`/`z` subscripts track the characteristic name, so the bitfield-to-characteristic mapping is
unambiguous — only the **characteristic-to-physical-channel** mapping is broken. **Resolve on hardware**
(e.g. drive one channel only and observe which electrode pair fires). Do not trust either reading.

Related, same table: V2 names the intensity field "Channel A/B intensity" with **A in bits 21–11 and B in bits
10–0**; whether that, too, is physically swapped is not independently verifiable from the docs.

### 6.5 Lower-severity wording gaps

- `coyote/README.md#L82` says *"the device discards all four pairs for that channel"*; `README_CN.md#L58` says
  *"脉冲主机会放弃掉该通道全部 4 组数据"* — both mean discard-all-4. Agrees, but note the docs elsewhere call
  the same construct "the channel's data", "4 pairs", and "4 sets"; there is no term for it that appears in
  both languages consistently. A "pair" is (frequency, intensity) covering 25 ms.
- Root README section heading `README.md#L17` says **"Coyote Stimulator"** while the per-version pages say
  "Coyote Stimulator V2/V3" and the Chinese root says "郊狼情趣脉冲主机" (Coyote *adult* pulse host) — cosmetic.
- `README.md#L50`: *"Documentation for V3 accessory interactions and how it will be shared is being revised."*
  So the V3 accessory surface (which is where `0xBE` presumably lived) is **explicitly acknowledged as
  incomplete** — this is the documented reason `0xBE` has no body.
- Changelog dates are in the future relative to the file mtimes (`2026/10/16` for the English docs added, while
  the files are dated 2025 in the working copy). Noted, not resolvable locally.

---

## 7. Safety-relevant constraints (stated or implied)

**Hard stated limits**

| Constraint | Value | Source |
|---|---|---|
| V3 channel intensity, absolute | 0–200 | `coyote/v3/README.md#L67` |
| V3 intensity-setting byte | 0–200, out-of-range → 0 | `coyote/v3/README.md#L67` |
| V3 channel intensity **soft limit** | 0–200, **persists across power cycles** | `coyote/v3/README.md#L123` |
| V3 soft limit, out-of-range write | leaves the existing limit unchanged | `coyote/v3/README.md#L123` |
| V3 Pulse frequency byte | 10–240, any invalid → whole channel block discarded | `coyote/v3/README.md#L80-L82` |
| V3 Pulse intensity byte | 0–100, any invalid → whole channel block discarded | `coyote/v3/README.md#L80-L82` |
| V3 balance parameters | 0–255, persist across power cycles | `coyote/v3/README.md#L129-L133` |
| V2 S | 0–2047, *"must not exceed 2047"* | `coyote/v2/README.md#L33` |
| V2 Z | 0–31; **`Z > 20` (width > 100 µs) more likely to sting** | `coyote/v2/README.md#L69-L71` |
| V3 waveform intensity vs V2 Z | V2 20 ≈ V3 100, so V2 Z 21–31 has no V3 equivalent | `coyote/README.md#L70`; `coyote/README.md#L86` |

**Soft limit is the last line of defence.** `coyote/v3/README.md#L125` verbatim: *"For example, with limits of
150 for A and 30 for B, the intensities remain within 0–150 and 0–30 respectively, whether changed using a Wheel
or a B0 command. Channel intensity cannot exceed its soft limit."* It cannot be bypassed by B0 — so setting it
is the only way to bound what your game can ever deliver. `[DER]` Because it persists across power cycles and
has no readback, an unknown stale limit is the documented hazard; the mitigation the docs give is to re-send BF
at every connection.

**When `0xBF` must be re-sent.** `coyote/v3/README.md#L113` verbatim: *"BF takes effect immediately and has no
response. Send BF to set the soft limits after every reconnection to prevent unexpected soft-limit values."*
`coyote/v3/README_CN.md#L86` is stronger: *"每次重连设备之后都必须重新写入 BF 指令设置软上限"* — **"must"**, with a
red-siren emoji and the same "no return value" warning. Both agree that BF is fire-and-forget.

`[GAP]` What is **not** stated: whether an out-of-band power cycle resets the soft limit to 200, to 0, or to
the stored value; whether the stored value can be read back at all (no read characteristic is documented for
it); whether writing BF mid-session can lower the limit below the current intensity (and what happens to the
channel if it does). The word "unexpected" in the warning implies the stored value survives in a way the host
cannot predict.

**Rate limits and pacing rules**

| Rule | Source |
|---|---|
| B0 is 20 bytes, *"Send this 20-byte command every 100 Ms"*, both channels in one command | `coyote/v3/README.md#L31` |
| Each of the 4 pairs per channel = 25 ms of output = 100 ms total | `coyote/v3/README.md#L82` |
| *"When changing intensity with a nonzero sequence number, wait for a B1 response with the matching sequence number before making another intensity change."* | `coyote/v3/README.md#L41` |
| V2: *"Each `[X,Y,Z]` set is valid for only 0.1 s … the device outputs the corresponding Pulse for 0.1 s and then stops."* | `coyote/v2/README.md#L75` |
| V2: parameter sets are sent *"one every 0.1 s"* in every example | `coyote/v2/README.md#L81-L97` |
| No other rate limit, cooldown, or duty-cycle rule is stated anywhere | `[GAP]` |

**Stability caveat the docs admit.** `coyote/README.md#L76-L81` verbatim:

> A Pulse frequency value can represent a duration longer than the output window. What happens when the next set
> of data arrives before that duration ends?
>
> The device handles this internally using a more complex process, whose details are outside the scope of this
> document. Keep the following in mind:
>
> 1. To maintain a stable Pulse frequency, keep sending the same value.
> 2. If the duration exceeds the output window and the next value differs, the resulting output may not directly
>    match either input value because of this internal processing.

`[DER]` Implication for a game-driven waveform: **do not treat the transmitted byte as an authoritative
frequency.** For any value above ~100 (output unit > 100 ms) you are past the window and the device's internal
behaviour is undefined by the docs. If you need a predictable sensation, stay near the short-duration end
(small byte values) and hold each value steady rather than modulating it every frame.

`[GAP]` Not documented, and worth confirming on hardware before shipping: whether the device zeroes channel
intensity on BLE disconnect or holds it (V2 says a written S *"remains in effect"*, `coyote/v2/README.md#L33`,
which hints at hold); whether B0 stops being required at any point; whether the device enforces any timeout on
B0 silence; whether more than one central may connect.

---

## 8. What is unresolved — checklist

| # | Unknown | Tag |
|---|---|---|
| 1 | Whether a WebSocket/relay/terminal transport exists at all | Not in this repo — absent |
| 2 | Any host/port/path/query, JSON schema, heartbeat, QR pairing payload, app name | Not in this repo — absent |
| 3 | Whether BF offsets 1/2, 3/4, 5/6 are A-then-B | `[GAP]` — inferred A-first |
| 4 | Whether V3 advertised names are exact or prefixes; advertising data vs GAP name | `[GAP]` |
| 5 | Whether pairing/bonding is required | `[GAP]` — not mentioned |
| 6 | Whether write-without-response is required, permitted, or irrelevant | `[GAP]` — not mentioned |
| 7 | ATT MTU: any need to negotiate | `[GAP]` — not mentioned; 20 bytes hints at default 23 |
| 8 | Integer division rounding in the frequency formula | `[GAP]` |
| 9 | V2 `PWM_A34`/`PWM_B34` → physical channel A/B mapping | `[CONFLICT]` — must be measured |
| 10 | V2 byte order (little- vs big-endian 24-bit) | `[GAP]` in prose, but `[DER]` little-endian from `example.md` |
| 11 | V2 S ↔ V3 intensity conversion | `[GAP]` — no formula given |
| 12 | Behaviour of `0xBE` | `[GAP]` — only "deprecated" is written |
| 13 | Device behaviour on disconnect, B0 silence, soft-limit reset | `[GAP]` |
| 14 | Whether B1 always reports both channels and fires for Wheel changes (sequence 0) | `[DER]` — strongly implied by `#L141`, not stated as a guarantee |
| 15 | What the device does if BF lowers the soft limit below the current intensity | `[GAP]` |

## 9. Practical consequences for the mod's transport layer

`[DER]` From the above, the only transport implementable from this repository is **BLE GATT direct with the PC
as central** — which also means no phone is required, and the AX210's BLE side is the adapter in play (no
statement here about whether the AX210's particular firmware/host stack is sufficient; the docs state no
adapter requirements at all).

Constraints the transport interface must express, because the V3 codec imposes them:

1. **A 100 ms pacing primitive** with both channels in one 20-byte frame. The transport cannot be
   "send this channel now"; it must be "emit frame N".
2. **A single-slot, acknowledged intensity path**, separate from the streaming waveform path: one intensity
   change in flight, cleared by an inbound `0xB1` whose sequence number matches. The waveform path must keep
   streaming sequence-0 frames while that change is pending.
3. **A fire-and-forget control path for `0xBF`**, re-issued on every connect, with no acknowledgement to wait
   for and no readback.
4. **An inbound notify stream** shared by B1 responses and unsolicited (sequence-0) intensity reports — the
   host must accept the latter without losing handshake state.
5. **Channel suppression expressed as an invalid value** (Pulse intensity 101) rather than an intensity of 0,
   since the two are documented to do different things.
6. **A hard clamp at the transport boundary** to V3 ranges (intensity setting 0–200, frequency byte 10–240,
   waveform intensity 0–100), plus a soft-limit write, because the device's own out-of-range handling is
   "silently becomes 0" for intensity and "discard the whole channel block" for waveform data — neither of
   which is a safe failure mode for a game-driven signal.
