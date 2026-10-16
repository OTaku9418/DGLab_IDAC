# IndRun.dll — Force-Feedback (FFB) API Surface: Static Reverse-Engineering Report

**Target:** `D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll`
**Size / form:** 2,634,240 bytes, PE32+ x64, no PDB, 123 exported mangled C++ symbols, TimeDateStamp `0x69392E86`.
**Cross-check binary:** `D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Binaries\Win64\GameProject-Win64-Shipping.exe` (69 MB).
**Method:** statically only — `pefile` for PE structure, `capstone` for disassembly, `.pdata`/EXCEPTION directory for function boundaries, raw RIP-displacement scanning for cross-references. The game was never executed and no file outside `D:\INDAC\DGLab_IDAC\` was written.

All addresses are **RVAs** unless written as a full VA. ImageBase = `0x180000000`, so RVA `0x5fa70` = VA `0x18005fa70`.

---

## 0. Executive summary

`IndRun.dll` is a UE4.24 game plugin that owns the cabinet's force-feedback *intent*. It does **not** talk to hardware at all — it imports no file, device, socket or thread API. Every FFB effect leaves the DLL through exactly **seven `std::function` callbacks** stored in one global singleton object, and the game (the EXE) supplies those callbacks. The game has **never** supplied the two centering callbacks, and IndRun never invokes them anyway.

That makes the seven binder exports and the five internal invoker trampolines the complete, hookable FFB surface. The single best hook point is the **torque invoker at RVA `0x21C0`**, which every torque decision funnels through with a `(direction, magnitude)` pair.

| Question | Answer |
|---|---|
| Where is FFB produced? | Inside one 100,820-byte function at RVA `0x148A0`–`0x2D274` (the whole game tick, fully inlined). Not hookable as a unit. |
| Where does it leave the DLL? | Five tiny invoker trampolines at RVA `0x2160`, `0x21A0`, `0x21C0`, `0x2200`, `0x2240`. |
| Can IndRun transmit FFB itself? | **No.** No `CreateFileW`/`WriteFile`/`DeviceIoControl`/socket/`CreateThread`/`Sleep`/`timeSetEvent` imports. |
| Who actually drives the wheel? | The EXE: it hosts a UE4 `/Script/SerialPort` module and imports `CreateFileW`/`WriteFile`/`GetCommState`/`SetCommState` → UART to the cabinet I/O board. |
| Is `idachook.dll` involved? | **No direct linkage.** Zero `idac`/`IDAC`/`idachook` strings in the EXE. `idachook.dll` lives on the AMDaemon side (`D:\INDAC\MODAC321\AMDaemon\idachook.dll`), a separate process. |
| Recommended hook | `IndRun.dll` RVA `0x21C0` (torque), plus `0x2200` (viscosity), `0x2240` (vibration), `0x2160`/`0x21A0` (servo on/off). |

---

## 1. PE structure and imports

**Sections**

| Section | VA | VirtualSize | RawPtr | RawSize | Characteristics |
|---|---|---|---|---|---|
| `.text` | `0x1000` | `0x88000` | `0x400` | `0x87a00` | `0x60000020` |
| `.rdata` | `0x89000` | `0x1b2000` | `0x87e00` | `0x1b1600` | `0x40000040` |
| `.data` | `0x23b000` | `0x7000` | `0x239400` | `0x2800` | `0xc0000040` |
| `.pdata` | `0x242000` | `0x45000` | `0x23bc00` | `0x45000` | `0x40000040` |
| `.rsrc` | `0x287000` | — | — | — | — |
| `.reloc` | `0x288000` | — | — | — | — |
| `.SCY` | `0x289000` | — | — | — | — |

EntryPoint RVA `0x86BE8`; SizeOfImage `0x28A000`; Subsystem 2 (GUI); DllCharacteristics `0x0060`; no TLS, no delay imports, no load config.

**Data directories**

| Directory | RVA | Size |
|---|---|---|
| EXPORT | `0x237910` | `0x1e60` |
| IMPORT | `0x2893C8` | `0x140` |
| EXCEPTION | `0x242000` | `0x30b4` |
| DEBUG | `0x22E2F0` | `0x70` |
| BASERELOC | `0x247000` | `0xa084` |

> Methodology note: the EXCEPTION directory size is `0x30B4`, i.e. **1039 `RUNTIME_FUNCTION` entries**, *not* the `.pdata` section's `0x45000` VirtualSize. Using the section size yields 19114 bogus entries and a broken function table — this was the single most important tooling correction in this analysis. All 1039 entries together span RVA `0x1060`–`0x88866`.

**Full import list** (enumerated with `pefile.PE(path, fast_load=True)` + `parse_data_directories()`; IAT at `0x180089000`+)

- **advapi32.dll** (1): `GetUserNameW`
- **kernel32.dll** (26): `GetCurrentProcess`, `OutputDebugStringW`, `DeleteCriticalSection`, `QueryPerformanceCounter`, `TlsFree`, `TlsAlloc`, `TlsSetValue`, `TlsGetValue`, `CreateDirectoryW`, `GetVersionExW`, `GetCurrentDirectoryW`, `GetComputerNameW`, `GlobalMemoryStatusEx`, `GetSystemTimeAsFileTime`, `RtlCaptureContext`, `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `UnhandledExceptionFilter`, `InitializeCriticalSection`, `GetCurrentThreadId`, `GetCurrentProcessId`, `IsDebuggerPresent`, `IsProcessorFeaturePresent`, `TerminateProcess`, `SetUnhandledExceptionFilter`, `InitializeSListHead`
- **msvcp140.dll** (3): `?_Xout_of_range@std@@YAXPEBD@Z`, `?_Xlength_error@std@@YAXPEBD@Z`, `?_Xbad_function_call@std@@YAXXZ` (IAT `0x1800890f8` — used by every `std::function` invoker's empty-check)
- **user32.dll** (1): `GetSystemMetrics`
- **vcruntime140.dll** (10): `memcpy`, `_CxxThrowException`, `__std_type_info_compare`, `memmove`, `memset`, `__std_type_info_destroy_list`, `_purecall`, `__C_specific_handler`, `__std_terminate`, `__CxxFrameHandler3`
- **winmm.dll** (3): `timeGetTime` (IAT `0x180089170`), `timeBeginPeriod` (`0x180089178`), `timeEndPeriod` (`0x180089180`)
- **ucrtbase.dll** (many): `atoi`, `_mkdir`, `_stat64i32`, `rename`, `remove`, `_aligned_free`, `_aligned_malloc` (IAT `0x1800891d0` — used for the FFB singleton allocation), `acosf`, `asinf`, `atan2f`, `cosf`, `tanf`, `sqrtf`, `floorf`, `fmodf`, `powf`, `sinf`, `_initterm`, `_initterm_e`, `_crt_atexit`, `_cexit`, `terminate`, `__stdio_common_vfprintf`, `vsprintf`, `vsnprintf_s`, `vsscanf`, `vfscanf`, `fopen`, `fopen_s`, `fread`, `fwrite`, `fclose`, `fgets`, `feof`, `fseek`, `ftell`, `rewind`, `strncpy`, `strcpy_s`, `strncmp`, `strcmp`, `wcsncmp`, `_time64`, `strftime`, `_localtime64`, `rand`

**Key negative result:** there is **no `CreateThread`, no `Sleep`, no `timeSetEvent`**. The only timing APIs are `timeGetTime` / `timeBeginPeriod` / `timeEndPeriod`. IndRun.dll creates no thread and no timer of its own.

---

## 2. Deliverable 1 — Full export inventory

123 ordinals, all under **one class: `cUE4Interface`**. 116 distinct RVAs; 6 exports are aliases of the shared unimplemented stub at **RVA `0x3DB0`** (`??1cUE4Interface@@QEAA@XZ` destructor, `?BindBuntaChallenge@…`, `?DebugCarSpeedUp@…`, `?DebugChangeMirrorAccessory@…`, `?DumpMemory@…`, `?ResetRace@…`).

The class is a **stateless facade**: the constructor `??0cUE4Interface@@QEAA@XZ` and the copy-assignment `??4cUE4Interface@@QEAAAEAV0@AEBV0@@Z` are *both* RVA `0x1480` — the object is empty, and all real state lives in file-scope globals. `this` is therefore irrelevant to the whole FFB API.

The complete demangled table is in **Appendix A**. The FFB / input-related subset:

| ord | RVA | demangled |
|----:|-----|-----------|
| 6 | `0x003db0` | `void cUE4Interface::BindBuntaChallenge(const std::function<void(?4, ?e, ?B, ?u, ?n, ?t, ?a, char, ?h, ?a, ?l, ?l, ?e, ?n, ?g, ?e, ?@, ?@)>&)` |
| 16 | `0x05ee60` | `void cUE4Interface::BindScreenPrintf(const std::function<void(?4, ?e, char, ?o, ?l, ?o, ?r, ?@, ?@, const char*)>&)` |
| 67 | `0x05efe0` | `void cUE4Interface::Pause(bool)` |
| 107 | `0x05eff0` | `void cUE4Interface::TimerPause(bool)` |
| 66 | `0x05f2a0` | `void cUE4Interface::OnTick(float)` |
| 89 | `0x05f8f0` | `void cUE4Interface::SetAccel(unsigned char)` |
| 91 | `0x05f920` | `void cUE4Interface::SetBrake(unsigned char)` |
| 99 | `0x05f950` | `void cUE4Interface::SetSteer(char)` |
| 97 | `0x05f970` | `void cUE4Interface::SetShift(char)` |
| 110 | `0x05fa60` | `void cUE4Interface::UnBindAllFB(void)` |
| 18 | `0x05fa70` | `void cUE4Interface::BindServoOn(std::function<void(void)>&&)` |
| 17 | `0x05fb10` | `void cUE4Interface::BindServoOff(std::function<void(void)>&&)` |
| 8 | `0x05fbb0` | `void cUE4Interface::BindCentering(std::function<void(void)>&&)` |
| 7 | `0x05fc60` | `void cUE4Interface::BindCentering(std::function<void(int)>&&)` |
| 19 | `0x05fd10` | `void cUE4Interface::BindTorqueFunction(std::function<void(nsFeedbackDefine::eTrqueDirection, int)>&&)` |
| 22 | `0x05fd80` | `void cUE4Interface::BindViscosityFunction(std::function<void(int, int)>&&)` |
| 21 | `0x05fe30` | `void cUE4Interface::BindVibrationFunction(std::function<void(int, int)>&&)` |
| 15 | `0x05fee0` | `void cUE4Interface::BindRaceTimerUpdater(const std::function<void(unsigned char, ?A, int)>&)` |
| 14 | `0x060050` | `void cUE4Interface::BindRaceInfoUpdater(const std::function<void(unsigned char, ?B, stRaceInfo&)>&)` |
| 13 | `0x0601c0` | `void cUE4Interface::BindRaceGoalEvent(const std::function<void()>&)` |
| 12 | `0x0601f0` | `void cUE4Interface::BindRaceFinishEvent(const std::function<void(unsigned char, ?B, stRaceExitState&)>&)` |
| 111 | `0x060700` | `void cUE4Interface::UnBindRace(void)` |
| 5 | `0x060950` | `void cUE4Interface::BindActorCarState(int, std::function<void(const stCarStateInfo&)>&&)` |
| 9 | `0x060b80` | `void cUE4Interface::BindCheckPointPassed(int, const std::function<void(int, int)>&)` |
| 20 | `0x060e00` | `void cUE4Interface::BindUDPSender(int, const std::function<void(unsigned char, ?A, void, int)>&)` |
| 11 | `0x060f20` | `void cUE4Interface::BindOnHitOtherCar(int, const std::function<void(unsigned char, ?B, stOnHitOtherCar&)>&)` |
| 109 | `0x0610c0` | `void cUE4Interface::UnBindActorEvent(int)` |
| 10 | `0x061430` | `void cUE4Interface::BindGetSoundUE4Func(const std::function<void(?A, ?V, ?c, ?S, ?o, ?u, ?n, ?d, E4&, void)>&)` |
| 94 | `0x0615d0` | `void cUE4Interface::SetRaceBrake(bool)` |
| 56 | `0x061b10` | `int cUE4Interface::GetSteeringAdjustment(void)  [static]` |


### FFB-relevant call-outs

| Role | Export | Ord | RVA |
|---|---|---|---|
| Bind torque callback | `void cUE4Interface::BindTorqueFunction(std::function<void(nsFeedbackDefine::eTrqueDirection, int)>&&)` | 19 | `0x5FD10` |
| Bind viscosity callback | `void cUE4Interface::BindViscosityFunction(std::function<void(int, int)>&&)` | 22 | `0x5FD80` |
| Bind vibration callback | `void cUE4Interface::BindVibrationFunction(std::function<void(int, int)>&&)` | 21 | `0x5FE30` |
| Bind centering — no-arg form | `void cUE4Interface::BindCentering(std::function<void()>&&)` | 8 | `0x5FBB0` |
| Bind centering — int form | `void cUE4Interface::BindCentering(std::function<void(int)>&&)` | 7 | `0x5FC60` |
| Bind servo on | `void cUE4Interface::BindServoOn(std::function<void()>&&)` | 18 | `0x5FA70` |
| Bind servo off | `void cUE4Interface::BindServoOff(std::function<void()>&&)` | 17 | `0x5FB10` |
| Unbind everything FFB | `void cUE4Interface::UnBindAllFB(void)` | 110 | `0x5FA60` |
| Per-frame tick (host-driven) | `void cUE4Interface::OnTick(float)` | 66 | `0x5F2A0` |
| Init (single entry) | `void cUE4Interface::Start(std::string const&, float)` | 103 | `0x5F030` |
| Steering input | `void cUE4Interface::SetSteer(char)` | 99 | `0x5F950` |
| Accel / brake / shift input | `SetAccel(unsigned char)` / `SetBrake(unsigned char)` / `SetShift(char)` | 89/91/97 | `0x5F8F0`/`0x5F920`/`0x5F970` |
| Static steering trim | `int cUE4Interface::GetSteeringAdjustment(void) [static]` | 56 | `0x61B10` |

Related binder families that share the identical mechanism but are not FFB: `BindScreenPrintf` (`0x5EE60`), `BindRaceTimerUpdater` (`0x5FEE0`), `BindRaceInfoUpdater` (`0x60050`), `BindRaceGoalEvent` (`0x601C0`), `BindRaceFinishEvent` (`0x601F0`), `bindEffectNotifer` (`0x60360`), `bindBGEffectNotifer` (`0x60390`), `bindFacialNotifer` (`0x603C0`), `bindRaceSuspendedNotifer` (`0x603F0`), `bindPreSaveGhost` (`0x60420`), `bindPostSaveGhost` (`0x60590`), `BindActorCarState` (`0x60950`), `BindCheckPointPassed` (`0x60B80`), `BindUDPSender` (`0x60E00`), `BindOnHitOtherCar` (`0x60F20`), `BindGetSoundUE4Func` (`0x61430`).

---

## 3. Deliverable 2 — The `nsFeedbackDefine` namespace

### 3.1 `enum nsFeedbackDefine::eTrqueDirection` (note the typo — "Trque")

**Type shape — PROVEN.** In the mangled name `W4eTrqueDirection@nsFeedbackDefine@@`, `W4` is the MSVC code for an `enum` whose underlying type is 4 bytes wide. The complete parameter encoding is `A6AXW4eTrqueDirection@nsFeedbackDefine@@H@Z`, i.e. return `X` = void, then `W4…` = the enum, then `H` = `int`. Therefore:

```cpp
namespace nsFeedbackDefine {
    enum eTrqueDirection : int;                      // 4-byte underlying type (PROVEN)
    typedef void (*TorqueCallback)(eTrqueDirection, int);   // PROVEN
}
```

**Enumerator values — PARTIALLY PROVEN.**

- **`-1` is PROVEN to be a valid enumerator.** Two independent sites use `0xFFFFFFFF` as the first argument of a torque callback:
  - RVA `0x2BCD6` — the normal per-frame path (see §4.2).
  - RVA `0x6D19` — the "zero the wheel" reset path (see §4.4): `torque(-1, 0)`.
- **`+1` is INFERRED** as the other value, by the following argument. The per-frame path is:

```
0002bccf  test      ebx, ebx
0002bcd1  mov       ecx, 0xffffffff     ; direction = -1
0002bcd6  jg        0x18002bcdb
0002bcd8  mov       ecx, r13d           ; direction = r13d   <-- the other value
0002bcdb  call      0x1800021c0         ; torque(ecx, edx)
```

  i.e. `direction = (torque > 0) ? -1 : <other>`. The function body writes `r13d` only three times, all `mov r13d, 1` (RVA `0x1B531`, `0x1E5D0`, `0x1E998`), and the only intervening 64-bit `r13` writes are pointer temporaries used as a `this` argument. The DLL also contains dedicated sign helpers that return exactly ±1 — `0x180004BD0` = `sign(int)` (`sar edx,0x1f; and edx,0xfffffffe; lea eax,[rdx+1]`) and `0x180004BE0` = `signum(float)` (returns `-1.0f` at `0x22C1D8` or `+1.0f` at `0x22B6D8`). **However, a full forward dataflow over the 100 KB function could NOT prove `r13d == 1` at `0x2BCD8`**: the function contains 3 indirect jumps whose tables cannot be resolved, the CFG is therefore incomplete, and backward reachability from `0x2BCD8` does not even reach the function entry. Treat `{ -1, +1 }` as a strong, code-supported inference and `-1` as the only *proven* member.

- **Enumerator NAMES are NOT RECOVERABLE statically.** IndRun.dll contains no enum-name strings and no RTTI for this type; the only occurrence of the identifier is inside the mangled export name itself. Do not expect `eTrqueDirection::Left`-style names.

**Do not confuse with `ESteeringFeedbackMode`.** The EXE defines a *different*, game-side enum `ESteeringFeedbackMode` with members, in order, `AutoCentering`, `Centering`, `Torque`, `Viscosity`, `Vibration` (EXE VA `0x2C72DF0`, so values 0–4). That is the game's own mode selector and has no mangled relationship to `eTrqueDirection`.

### 3.2 Struct/class layout of the feedback types

No user-defined struct is passed across the FFB API — every callback parameter is a scalar (`eTrqueDirection`, `int`, `void`). The only structured object is the **internal FFB singleton**, layout fully proven (see §4.1). The many `st…` structs in the export list (`stRaceInfo`, `stCarStateInfo`, `stRaceExitState`, `stOnHitOtherCar`, `stCourseSetting`, `stRaceSetting`, `stCharacterSetting`, `stMirrorAccessorySetting`, `stTestModeToConfig`) belong to the race/telemetry API, not to FFB; their layouts were not recovered and are out of scope.

Other types in the namespace: nothing recoverable. `nsFeedbackDefine` appears exactly once in the image — inside the `BindTorqueFunction` mangled name.

---

## 4. Deliverable 3 — Signature semantics (PRIORITY)

### 4.1 The storage mechanism — everything funnels into one global singleton

Every `Bind*` export has the same shape: `rcx` = `this` (**never read**), `rdx` = `std::function&&`. The function copies the incoming `std::function` to a 0x40-byte stack temporary, **move-assigns** it into a slot of the object pointed to by the global at **RVA `0x241890`**, then destroys the caller's temporary.

MSVC x64 `std::function` layout, confirmed empirically: **size `0x40`**, impl pointer `_Ptr` at **`+0x38`**; impl vtable slots `[0x00]=_Copy`, `[0x08]=_Move`, `[0x10]=_Do_call`, `[0x18]=_Target_type`, `[0x20]=_Delete_this`.

The singleton's construction is proven at RVA `0x5E084`–`0x5E0C3`:

```
18005e084  mov  qword ptr [rax], rcx         ; *(g+0x00) = rcx
18005e087  mov  qword ptr [rax + 0x40], rbp  ; g+0x40  = 0   (ServoOn   _Ptr, slot g+0x08)
18005e08b  mov  qword ptr [rax + 0x80], rbp  ; g+0x80  = 0   (ServoOff  _Ptr, slot g+0x48)
18005e092  mov  qword ptr [rax + 0xc0], rbp  ; g+0xc0  = 0   (Center()  _Ptr, slot g+0x88)
18005e099  mov  qword ptr [rax + 0x100], rbp ; g+0x100 = 0   (Center(i) _Ptr, slot g+0xc8)
18005e0a0  mov  qword ptr [rax + 0x140], rbp ; g+0x140 = 0   (Torque    _Ptr, slot g+0x108)
18005e0a7  mov  qword ptr [rax + 0x180], rbp ; g+0x180 = 0   (Viscosity _Ptr, slot g+0x148)
18005e0ae  mov  qword ptr [rax + 0x1c0], rbp ; g+0x1c0 = 0   (Vibration _Ptr, slot g+0x188)
18005e0ba  cmp  qword ptr [rip + 0x1e37cf], rbp   ; lazy-init guard on [0x241890]
18005e0c1  jne  0x18005e0ca
18005e0c3  mov  qword ptr [rip + 0x1e37c6], rax   ; [0x241890] = rax   <-- singleton store
```

**Complete slot map of `g = *(0x180241890)`** (each slot is a `std::function`, `_Ptr` at `slot+0x38`):

| Slot | Writer (export) | Signature |
|---|---|---|
| `g+0x08` | `BindServoOn` `0x5FA70` | `std::function<void()>` |
| `g+0x48` | `BindServoOff` `0x5FB10` | `std::function<void()>` |
| `g+0x88` | `BindCentering` `0x5FBB0` | `std::function<void()>` |
| `g+0xC8` | `BindCentering` `0x5FC60` | `std::function<void(int)>` |
| `g+0x108` | `BindTorqueFunction` `0x5FD10` | `std::function<void(nsFeedbackDefine::eTrqueDirection, int)>` |
| `g+0x148` | `BindViscosityFunction` `0x5FD80` | `std::function<void(int, int)>` |
| `g+0x188` | `BindVibrationFunction` `0x5FE30` | `std::function<void(int, int)>` |

Two move-assign helpers implement this: `0x180002400` is the generic `std::function::operator=(function&&)` (`rcx` = dst, `rdx` = src); `0x180002280` is a torque-specialised copy hard-wired to `dst = [0x241890] + 0x108`.

`UnBindAllFB` (`0x180002020`) walks exactly those seven slots in order and, for each, does `rcx = [slot+0x38]; if (rcx) { rax=[rcx]; dl = (rcx != &slot); call [rax+0x20] /* _Delete_this */ } ; [slot+0x38] = 0`.

### 4.2 The invoker trampolines — where the bound `std::function` is actually called

These five routines are the **only** code in the image that reads the FFB slots. Arguments are passed **by pointer** (first callback parameter in `rdx`, second in `r8`), matching the `_Do_call` calling convention of a `std::function` invoked on `void` return.

**Torque invoker — RVA `0x21C0`** (the recommended hook):

```
1800021c0  mov   eax, ecx                  ; arg1 = direction
1800021c2  mov   rcx, qword ptr [0x241890] ; g
1800021c9  test  rcx, rcx
1800021cc  je    ret
1800021ce  mov   rcx, qword ptr [rcx+0x140]; _Ptr of slot g+0x108
1800021d5  test  rcx, rcx
1800021d8  je    ret
1800021da  mov   dword ptr [rsp+0x20], edx ; arg2 = magnitude
1800021de  lea   r8,  [rsp+0x20]
1800021e2  mov   dword ptr [rsp+0x28], eax ; arg1 = direction
1800021e6  lea   rdx, [rsp+0x28]
1800021ea  mov   rax, qword ptr [rcx]      ; impl vtable
1800021ed  call  qword ptr [rax+0x10]      ; _Do_call
1800021f0  ret
```

The call is a plain `call`, **not a tail `jmp`**, so the args are still live on the stack at the call site — good for hooking.

**Viscosity invoker — RVA `0x2200`**: identical shape, reads `[g+0x180]` (= slot `g+0x148`). `(a, b)` = `(ecx, edx)`.
**Vibration invoker — RVA `0x2240`**: identical shape, reads `[g+0x1C0]` (= slot `g+0x188`). `(a, b)` = `(ecx, edx)`.

**Servo-ON invoker — RVA `0x2160`** (note the deliberate two-slot gate):

```
180002160  sub  rsp, 0x28
180002164  mov  rax, [0x241890]
18000216b  test rax, rax
18000216e  je   ret
180002170  cmp  qword ptr [rax+0x80], 0    ; *** tests ServoOFF (slot g+0x48) ***
180002175  je   ret
180002177  mov  rcx, qword ptr [rax+0x40]  ; then calls ServoON (slot g+0x08)
18000217b  test rcx, rcx
180002181  je   __std_bad_function_call
180002187  mov  rax, qword ptr [rcx]
18000218a  jmp  qword ptr [rax+0x10]       ; tail-call _Do_call
```

Read as `if (servoOff.is_set() && servoOn.is_set()) servoOn();` — i.e. "only drive the servo if the game has armed the FFB pair".

**Servo-OFF invoker — RVA `0x21A0`**: `rax=[g]; if(!rax) ret; rcx=[rax+0x80]; if(!rcx) ret; rax=[rcx]; jmp [rax+0x10]` (no arguments).

**There is NO invoker for slots `g+0x88` or `g+0xC8` anywhere in the image.** Only the two binders and `UnBindAllFB` ever touch them. This is independently corroborated by the EXE, which contains **zero** occurrences of `?BindCentering` — the shipping game never supplies a centering callback. Any centering effect therefore must be produced by the game's own centering logic, not by this API.

### 4.3 Call sites of the invokers, and the torque computation

All five per-frame invocations live inside **one `RUNTIME_FUNCTION`, RVA `0x148A0`–`0x2D274` (100,820 bytes)** — the entire game tick, everything inlined. That function is called from exactly one place (`0xEFE7`, see §5).

| Call site | Invoker | Semantics |
|---|---|---|
| `0x2B7B8` | `0x2160` servo-on | gated by `[rdi+0x5812]!=0 && [rdi+0x4E8]==0x0A && [rdi+0x164D]==0`; immediately followed by `0x2B7C1 call 0x2240` → `vibration(0,0)` |
| `0x2B7E3` / `0x2B7EC` | `0x2200` / `0x2240` | `viscosity(0,0)` then `vibration(0,0)`; gated by `[rdi+0x4EED]!=0 && xmm6 > [car+0x94C]` |
| `0x2BCDB` | `0x21C0` | **torque** — the main per-frame torque |
| `0x2BE91` | `0x2240` | vibration |
| `0x2BFC1` | `0x2200` | viscosity |
| `0x0C053` | `0x21A0` | servo off |

**The torque call, with the log line that proves the parameter meaning:**

```
18002bcc6  mov       ecx, ebx                       ; ebx = computed signed torque
18002bcc8  call      0x1800390f0                    ; abs(int): eax=ecx; cdq; xor eax,edx; sub eax,edx
18002bccd  mov       edx, eax                       ; edx = |torque|  = MAGNITUDE
18002bccf  test      ebx, ebx
18002bcd1  mov       ecx, 0xffffffff                ; direction = -1
18002bcd6  jg        0x18002bcdb
18002bcd8  mov       ecx, r13d                      ; direction = +1 (inferred)
18002bcdb  call      0x1800021c0                    ; torque(direction, magnitude)
18002bce0  mov       dword ptr [rdi+0x163c], ebx    ; cache last torque
18002bce6  movsd     qword ptr [rsp+0x30], xmm6     ; vararg 2 (double) = kickback force
18002bcec  mov       dword ptr [rsp+0x28], ebx      ; vararg 1 (int, %d)  = torque
18002bcf0  lea       rax, [rip+0x1ea2d1]           ; [0x215FC8]
18002bcf7  mov       qword ptr [rsp+0x20], rax
18002bcfc  mov       r9d, 7
18002bd02  xor       r8d, r8d
18002bd05  xor       edx, edx
18002bd07  mov       rcx, qword ptr [rip+0x215b72]  ; [0x241880] logger singleton
18002bd0e  call      0x180083cd0                    ; printf-style sink
```

The format string begins at **RVA `0x215FC8`** and is exactly:

```
"------------------- Torque %d KickBackForce %.3f"
```

(hexdump context: `"[%.1f %.1f %.1f %.1f]\0\0\0QuickPlay %d %d\0"` immediately precedes it). Its varargs are, in order, `ebx` (the signed torque, `%d`) and `xmm6` (a double, `%.3f`). This is the single strongest piece of evidence for parameter meaning: **the same signed integer that is logged as `Torque %d` is the value whose absolute value becomes the callback's second argument and whose sign selects the first argument.** Hence:

> **`BindTorqueFunction`'s callback receives `(direction, magnitude)` where `magnitude = abs(signed_torque)` and `direction ∈ {-1, +1}` selects the sign.** The magnitude is **not** a 0–255 duty value and **not** a duration: it is a signed-torque-derived integer that the code clamps to **±127** before use (see §4.5), i.e. a **signed 8-bit-range force value**, with direction carried separately.

**Vibration call — RVA `0x2BE91`:**

```
0002be6c  call      0x180004bc0        ; clamp(xmm2, xmm1, xmm3)  (maxss xmm3,xmm1; minss xmm2,xmm3)
0002be71  cvttss2si ebx, xmm0         ; arg1 = (int)clamped value
0002be75  cmp       dword ptr [rdi+0x1640], ebx   ; change detection vs cached value
0002be7b  je        0x18002be99
0002be7d  mulss     xmm7, xmm12
0002be82  cmp       byte ptr [rdi+0x5812], 0      ; *** master FFB enable flag ***
0002be89  je        0x18002be96
0002be8b  cvttss2si edx, xmm7         ; arg2 = (int)(xmm7 * xmm12)
0002be8f  mov       ecx, ebx
0002be91  call      0x180002240        ; vibration(arg1, arg2)
0002be99  mov       dword ptr [rdi+0x1640], ebx   ; update cache
```

**Viscosity call — RVA `0x2BFC1`:**

```
0002bf9d  call      0x180004bc0        ; clamp
0002bfa2  cvttss2si ebx, xmm0         ; arg2
0002bfa6  cmp       dword ptr [rdi+0x1644], ebx
0002bfac  jne       0x18002bfb3
0002bfae  test      r14b, r14b
0002bfb1  je        0x18002bfc6
0002bfb3  cmp       byte ptr [rdi+0x5812], 0      ; master FFB enable flag
0002bfba  je        0x18002bfc6
0002bfbc  mov       edx, ebx
0002bfbe  mov       ecx, r13d          ; arg1 = r13d (±1, same value as torque direction)
0002bfc1  call      0x180002200        ; viscosity(arg1, arg2)
0002bfc6  mov       dword ptr [rdi+0x1644], ebx
```

Both are **edge-triggered**: a cached value at `[manager+0x1640]` (vibration) / `[manager+0x1644]` (viscosity) suppresses redundant calls, and both are gated on the master FFB-enable byte `[manager+0x5812]`. `rdi` is the IndRun manager object; `[rdi+0x3B58]` is a pointer to the vehicle/car state block.

Helpers used above, disassembled and named:

| RVA | Identity | Disassembly |
|---|---|---|
| `0x390F0` | `abs(int)` | `mov eax,ecx; cdq; xor eax,edx; sub eax,edx; ret` |
| `0x4B80` | `clamp_int(v, lo, hi)` | `cmp edx,r9d; cmovl edx,r9d; cmp edx,r8d; cmovg edx,r8d; mov eax,edx` |
| `0x4BC0` | `clamp_float(hi, lo, v)` | `maxss xmm3,xmm1; minss xmm2,xmm3; movaps xmm0,xmm2` |
| `0x4BD0` | `sign(int)` → ±1 | `sar edx,0x1f; and edx,0xfffffffe; lea eax,[rdx+1]` |
| `0x4BE0` | `signum(float)` → ±1.0f | `xorps xmm0,xmm0; comiss xmm0,xmm1; jbe → [+1.0f @0x22B6D8] else [-1.0f @0x22C1D8]` |
| `0x83CD0` | logging sink | `(rcx=[0x241880], rdx, r8, r9d=severity, fmt, ...)` |

### 4.4 The FFB reset path (proves the zero/neutral encoding)

When a race ends, RVA `0x6C7E`–`0x6D48` zeroes each effect:

```
180006c7e  rax = [0x241890]                    ; if null, bail
180006c8e  cmp [rax+0x180], 0 / je skip        ; viscosity _Ptr
180006c97  [rsp+0x30]=0 ; [rsp+0x38]=0
180006ca6  rcx=[rax+0x180]; rdx=&[rsp+0x38]; r8=&[rsp+0x30]; call [rcx][0x10]   ; viscosity(0, 0)
180006ccf  cmp [rax+0x1c0], 0 / je skip        ; vibration _Ptr
180006cd8  args = 0, 0 ; call [rcx][0x10]                                        ; vibration(0, 0)
180006d10  cmp [rax+0x140], 0 / je end         ; torque _Ptr
180006d19  [rsp+0x50]=0 ; [rsp+0x28]=0xffffffff
180006d25  rcx=[rax+0x140]; rdx=&[rsp+0x28]; r8=&[rsp+0x50]; call [rcx][0x10]    ; torque(-1, 0)
180006d49  rax=[0x241898]; byte [rax+0x10]=sil
```

`torque(-1, 0)` and `viscosity(0,0)` / `vibration(0,0)` are the official "off" encodings — useful for a mod that wants to neutralise the wheel.

### 4.5 Every numeric constant in the FFB region

Floats are stored in `.rdata` and loaded RIP-relative; each was resolved to its address and read as IEEE-754 binary32/binary64.

| Constant | Address | Site | Context |
|---|---|---|---|
| `9.99999993922529e-09` | `0x22B16C` | `0x2BF6E` | comparison epsilon (**float32 min-normal / "≈0"**) |
| `1.0e-05` | `0x22B180` | `0x2B8F7` | epsilon |
| `0.001` | `0x22B1C0` | `0x2BFCC` | epsilon |
| `0.01` | `0x22B208` | `0x2BF11` | epsilon / dead-zone |
| `0.05` | `0x22B264` | `0x2B96A`, `0x2BEAE` | threshold |
| `0.1` | `0x22B2DC` | `0x2B977`, `0x2BAF5`, `0x2BFDE` | scale |
| `0.138` | `0x22B308` | `0x2B9BB` | `subss` bias |
| `0.2` | `0x22B350` | `0x2B8B6` | scale |
| `0.4` | `0x22B3C4` | `0x2B892`, `0x2B9F2` | **torque scale** |
| `0.6` (≈0.60000002384) | `0x22B458` | `0x2BC1E`, `0x2BC67` | **torque scale (mid)** |
| `0.8` (f64) | `0x22BAB8` | `0x2BB58`, `0x2BBAD` | lerp weight |
| `1.0` (f64) | `0x22BB68` | `0x2BA90` | normalisation |
| `1.0` (f32) | `0x22B6D8` | `0xEF4E`, `0xEF91` | **FFB gain default / signum(+)` |
| `1.1` | `0x22B800` | `0x2BA68` | gain |
| `1.2` | `0x22B890` | `0x2BF8E` | gain |
| `4.0` | `0x22BDC0` | `0x2BF30` | threshold |
| `5.0` | `0x22BDD4` | `0x2BD95` | threshold |
| `6.0` | `0x22BDE0` | `0x2B889` | **kickback scale** |
| `8.0` | `0x22BE1C` | `0x2BE2E` | threshold |
| `10.0` | `0x22BE3C` | `0x2BD2A` | **kickback scale** |
| `15.0` | `0x22BE5C` | `0x2BAB1` | scale |
| `20.0` | `0x22BE78` | `0x2BD73` | **kickback scale** |
| `32.0` | `0x22BEAC` | `0x2BEF5` | **vibration scale** |
| `40.0` | `0x22BEC0` | `0x2BEC5`, `0x2BF43` | **vibration scale** |
| `58.8` | `0x22BEF0` | `0x2BD6B` | **kickback scale (58.8 — a tuned magic number)** |
| `60.0` | `0x22BEF8` | `0x2BFD4` | **kickback scale** |
| `85.0` (f64) | `0x22BD88` | `0x2BB1C` | **kickback scale** |
| `100.0` | `0x22BF30` | `0x2BC10`, `0x2BC5E`, `0x2C01C` | **normalisation / 100 %** |
| `120.0` | `0x22BF44` | `0x2BE5E` | **vibration scale** |
| `124.0` | `0x22BF48` | `0x2B9D5` | **kickback scale** |
| `200.0` | `0x22BF80` | `0x2BD22` | **kickback scale** |
| `350.0` | `0x22BFA8` | `0x2BD9D` | **kickback scale (largest)** |
| `-1.0` (f32) | `0x22C1D8` | `0x4BE8` | `signum` negative result |

Integer constants and structural constants:

| Value | Site | Context |
|---|---|---|
| `0xFFFFFF81` = **−127** | `0x2BC46` (`mov r9d,0xffffff81`) | torque clamp **lower** bound → helper `0x4B80` |
| `0x7F` = **+127** | `0x2BC4C` (`mov r8d,0x7f`) | torque clamp **upper** bound |
| `0x0A` = 10, `0x14` = 20, `0x3C` = 60 | `lea r8d,[r9+N]` | upper bounds to the `clamp_int` helper `0x4B80` |
| `0x3C` = 60 | `0x2B8xx` `mov dword ptr [rdi+0x4F0], 0x3C`; also `0x1E892 mov dword ptr [rax+0x15E8], 0x3C` | per-effect countdown / hold timer (frames) |
| `0xF000` masked and compared to `0x1000` | `0x2BC80` | bitmask/flag test on a state word |
| `1.0f` / `0.6f` / `0.3f` written to `[manager+0x2C48]` | `0x2Bxxx` | **torque-scale selection table** (three discrete levels) |

**Runtime-configurable FFB tunables (BSS, zero in the file — written at runtime):**

| Address | Read at | Meaning |
|---|---|---|
| `0x24113C` | `0xEF83` | default-case FFB gain |
| `0x241140` | `0xEFD0` | FFB gain, mode case 2 |
| `0x241144` | `0xEFBE` | FFB gain, mode case 3 |
| `0x241148` | `0xEFAC` | FFB gain, mode case 4 |
| `0x24114C` | `0xEF9A` | FFB gain, mode case 5 |
| `0x2412F0`, `0x2412F4`, `0x2412F8` | `0x2BAC2`, `0x2BAAD`, `0x2BA83` | torque-curve coefficients |
| `0x241304` | `0x2BB6E`, `0x2BBF2` | torque-curve coefficient |
| `0x24133C` | `0x2B8A0` | torque-curve coefficient |
| `0x241484` | `0xEFF0` | kickback gate threshold |

Each of these has exactly **one** read site and **no** write site inside IndRun.dll, and they are zero in the file image — so they are populated at runtime (config load through `?ReloadConfig@cUE4Interface@@QEAAXXZ` at `0x5EFC0`, or written by the host). A PC-side mod that wants to retune FFB can simply write these floats in the loaded module rather than hooking anything.

**Manager/vehicle fields frequently read by the FFB code.** `rdi` = IndRun manager; `[rdi+0x3B58]` = vehicle-state pointer; `[manager+0x4E4]` = this tick's FFB gain (see §5). Manager: `+0x4CC`, `+0x4E8`, `+0x4EC`/`+0x4ED`/`+0x4EF0`/`+0x4EF5`/`+0x4EF8`, `+0x4F0`, `+0x4F8`, `+0x163C` (last torque), `+0x1640` (last vibration), `+0x1644` (last viscosity), `+0x1648`, `+0x164C`, `+0x164D`, `+0x2C48` (torque scale), `+0x51E8`, `+0x51EC`, `+0x51F0`, `+0x51F4`, `+0x51F8`, `+0x53E8`, `+0x5808`, `+0x5812` (**master FFB enable**), `+0x5838`, `+0x583C`, `+0x5840`. Vehicle: `+0x000`, `+0x008`, `+0x1B4`, `+0x1B8`, `+0x1E4`, `+0x1EC`, `+0x200`, `+0x248/0x249/0x24C`, `+0x258/0x259`, `+0x26C`, `+0x310`, `+0x344`, `+0x39C`, `+0x8AC`, `+0x8B4`, `+0x934`, `+0x93C`, `+0x94C`, `+0x958`, `+0x998`, `+0xA84`, `+0xC5C`, `+0xC88` (**assist/mode selector, see §5**), `+0xCFC`, `+0xD44`, `+0xDC8`–`+0xDCB`, `+0xE08`, `+0xE10`.

---

## 5. Deliverable 5 — Initialization path

### 5.1 There is exactly one initialization entry point

**`?Start@cUE4Interface@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@M@Z`** — ordinal **103**, RVA **`0x5F030`**, signature `void cUE4Interface::Start(std::string const&, float)`.

It is the only caller of the internal initializer **fn#466, RVA `0x5DD30`–`0x5E227`**, from the single site **RVA `0x5F09B`**. That initializer does, in order:

1. Allocate the FFB singleton via `_aligned_malloc(0x98, 8)` at `0x5E0CA` (`mov edx,8; mov ecx,0x98; call [_aligned_malloc]`).
2. Store it to `[0x241890]` at `0x5E0C3` after zeroing all seven `std::function` `_Ptr` fields (`0x5E087`–`0x5E0AE`).
3. Allocate a second, vtable-bearing object (vftables `0x229E00`, `0x229E10`, `0x229DD0`) stored to globals `0x2418C8` and `0x2418B8`.
4. Capture `timeGetTime()` into globals `0x23E16C` and `0x23E170` at `0x5E14E`.
5. Call `timeBeginPeriod(1)` (`mov ecx,1; call [timeBeginPeriod]`) — raising the system timer resolution to **1 ms**.

Teardown at `0x5E2A4` writes `[0x241890] = 0`.

**So a mod needs exactly one hook for lifecycle: RVA `0x5F030` (`?Start@cUE4Interface@@…`).** Note that `Bind*` may be called before or after `Start`; the binders fetch `[0x241890]` at call time, so they require the singleton to exist.

### 5.2 Who calls `BindTorqueFunction`

The **host EXE**, dynamically, by name. Evidence:

- `GameProject-Win64-Shipping.exe`'s static import table contains **43 DLLs and no `IndRun.dll` entry** — so the plugin is not link-time imported.
- The EXE imports `GetProcAddress` (12 uses) and `LoadLibraryW`/`LoadLibrary` (17 uses).
- The EXE's `.rdata` contains a length-prefixed name list at VA `0x143924BF1` (file `0x39241F1`) holding the exact mangled strings `?BindTorqueFunction@cUE4Interface@@QEAAXV?$function@$$A6AXW4eTrqueDirection@nsFeedbackDefine@@H@Z@std@@@Z`, `?BindViscosityFunction@…`, `?BindVibrationFunction@…`, `?BindServoOn@…`, `?BindServoOff@…`, `?UnBindAllFB@…` (102 `cUE4Interface` name hits in total).
- The EXE also carries `IndRun.dll` at VA `0x1433784A0` and `/Script/IndRun` at `0x142A9A960`.

**Caveat:** those six name strings have **zero** RIP-relative code cross-references and **zero** absolute-pointer references in the EXE. They live in a blob that is walked at runtime (an index/offset table), so the game's `Bind*` call sites cannot be located by xref-to-string. Locating the exact call site would need a debugger or a dynamic trace, which is outside this static-only scope.

### 5.3 The per-tick FFB wrapper (the other useful hook point)

The mega-function at `0x148A0` is called from **exactly one** site — RVA `0xEFE7` — inside `RUNTIME_FUNCTION` **#118, RVA `0xEF15`–`0xEFFE`**:

```
0000ef15  movd      xmm0, dword ptr [rbx+0x4fe8]
0000ef1d  cvtdq2ps  xmm0, xmm0
0000ef20  addss     xmm0, dword ptr [rbx+0x5840]
0000ef28  comiss    xmm0, dword ptr [rbx+0x583c]
0000ef2f  movss     dword ptr [rbx+0x5840], xmm0
0000ef37  jb        0x18000ef47
0000ef39  mov       byte ptr [rbx+0x4ef0], 1
0000ef40  mov       byte ptr [rbx+0x4eec], 0
0000ef47  mov       rax, qword ptr [rbx+0x3b58]        ; vehicle state
0000ef4e  movss     xmm7, dword ptr [0x22b6d8]        ; xmm7 = 1.0f
0000ef56  mov       dword ptr [rbx+0x4e4], 0x3f800000 ; default FFB gain = 1.0f
0000ef60  mov       ecx, dword ptr [rax+0xc88]        ; *** assist / mode selector ***
0000ef66  sub       ecx, 2  / je 0xefcc              ; case 2 -> gain = [0x241140] * [rbx+0x51ec]
0000ef6b  sub       ecx, 1  / je 0xefba              ; case 3 -> gain = [0x241144] * [rbx+0x51f0]
0000ef70  sub       ecx, 1  / je 0xefa8              ; case 4 -> gain = [0x241148] * [rbx+0x51f4]
0000ef75  sub       ecx, 1  / je 0xef96              ; case 5 -> gain = [0x24114c] * [rbx+0x51f8]
0000ef7a  cmp ecx,1 / je 0xef91                     ; case 6 -> gain = 1.0f
0000ef7f  default -> gain = [0x24113c] * [rbx+0x51e8]
0000efdc  mov       rcx, rbx
0000efdf  movss     dword ptr [rbx+0x4e4], xmm0       ; store this tick's FFB gain
0000efe7  call      0x1800148a0                        ; *** ALL FFB HAPPENS INSIDE HERE ***
```

`[vehicle+0xC88]` is the steering-assist / feedback mode selector, and the five `.data` floats `0x24113C`, `0x241140`, `0x241144`, `0x241148`, `0x24114C` form the per-mode gain table, each multiplied by a per-car value (`[manager+0x51E8/0x51EC/0x51F0/0x51F4/0x51F8]`). The result lands in `[manager+0x4E4]`, read by the FFB code inside the mega-function.

RVA `0xEF15` has **no direct callers**: it is reached by fall-through/jump from the preceding `RUNTIME_FUNCTION` fragment `0xECDF`–`0xEF15` (verified: `0xEF13 je 0x18000EF47`), which is itself a continuation of a longer fragment chain entered from RVA `0x50220` region (`0x50379 call 0xE870`, `0x503B7 jcc 0xF270`, `0x503D2 call 0xF590`). MSVC split this logical function into many `RUNTIME_FUNCTION` fragments, which is why a naive "find the callers of 0xEF15" search returns nothing.

---

## 6. Deliverable 4 — Threading and timing

**No thread, no timer, no sleep.** IndRun.dll imports no `CreateThread`, no `Sleep`, no `timeSetEvent`, no `SetTimer`, no `WaitForSingleObject`. Its complete timing toolkit is `timeGetTime`, `timeBeginPeriod`, `timeEndPeriod` and `QueryPerformanceCounter` (the last only for the crash-handler/telemetry path).

**The driver is the host's per-frame tick.** The exported entry is

**`void cUE4Interface::OnTick(float)` — ordinal 66, RVA `0x5F2A0`.**

It has **zero callers inside IndRun.dll**, so it is called by the EXE. Its prologue is decisive:

```
0005f2a0  push      rdi
0005f2a2  sub       rsp, 0x20
0005f2a6  mov       rdi, qword ptr [rip+0x1e222b]   ; [0x2414d8]
0005f2ad  call      qword ptr [rip+0x29ebd]        ; *** IAT 0x180089170 = timeGetTime ***
0005f2b3  mov       rdx, qword ptr [rip+0x1e25e6]   ; [0x2418a0]
0005f2ba  mov       dword ptr [rip+0x1deeac], eax  ; *** [0x23E16C] = timeGetTime() ***
```

`OnTick` **ignores its `float` argument** (xmm0 is never read) and instead stamps the tick with its own `timeGetTime()` millisecond timestamp into global `0x23E16C`. `OnTick` then drives subsystems through `0x18007C950`, `0x180079C30` and `0x18006A240`.

**Update interval — what can and cannot be proven.**

- **Cannot be proven statically:** the tick period. It is whatever rate the host EXE calls `OnTick`/the internal update at, and neither IndRun.dll nor the EXE exposes it as a literal. There is **no `Config` directory** on disk under `WindowsNoEditor\GameProject\` (only `Binaries`, `Content`, `Plugins`), so `DefaultEngine.ini` — where UE4 stores `bUseFixedFrameRate`/`FixedFrameRate`/`SmoothedFrameRateRange` — is inside the `.pak`, not readable as a file.
- **Can be proven:** `timeBeginPeriod(1)` is called during initialization (`0x5E14E` region), so the process runs with **1 ms system timer resolution** — consistent with a sub-10 ms control loop and with the FFB code's reliance on `timeGetTime()` deltas.
- **Adjacent but distinct:** a second pair of millisecond timestamps is maintained at `0x23E16C`/`0x23E170`, and a delta is computed elsewhere as `eax = [0x23E170]; eax -= [0x23E16C]` at RVA `0x74E8A`–`0x74EA7` inside `RUNTIME_FUNCTION` #757 (`0x74B90`–`0x75C71`), then reduced with `and eax, 7` and stored to `[manager+0x430]`.

**Practical answer:** treat the FFB update rate as *the game's frame/tick rate*, and measure it at runtime rather than relying on a static constant. The `[manager+0x1640]`/`[manager+0x1644]` change-detection caches mean the callbacks only fire when a value actually changes, so observed callback frequency is ≤ tick rate.

---

## 7. Deliverable 6 — Hook recommendation (PRIORITY)

### 7.1 Recommendation

**Hook `IndRun.dll`, not `idachook.dll`.** Justification:

- `idachook.dll` has **no linkage** to this game: the EXE contains zero `idac`/`IDAC`/`idachook` strings, and `idachook.dll` lives on the AMDaemon side (`D:\INDAC\MODAC321\AMDaemon\idachook.dll`) in a separate process. Hooking it would intercept a different process's abstraction.
- `IndRun.dll` is where the *semantic* FFB intent lives: `(direction, magnitude)` in engineering units, before any device-specific encoding. That is the highest-value observation point for a mod.
- IndRun.dll is a plugin loaded with `LoadLibrary`, so it can be hooked after the fact, and the singleton pointer `*(0x180241890)` gives direct access to the seven live callbacks for both reading *and* replacing.

### 7.2 Primary hook: the torque invoker, RVA `0x21C0`

| Property | Value |
|---|---|
| RVA | **`0x21C0`** (VA `0x1800021c0`) |
| File offset | `0x21C0` − `0x1000` + `0x400` = **`0x15C0`** |
| Calling convention | **`__fastcall`/MSVC x64**: `rcx` = `eTrqueDirection` (low 32 bits of `rcx` used), `rdx` = `int` magnitude |
| Return | `void` (but the function leaves `rax` clobbered by the callback; do not rely on it) |
| What the hook receives | `int direction` — proven value `-1`; inferred `+1`. `int magnitude` = `abs(signed_torque)`, clamped to **±127 elsewhere** in the torque computation |
| Stack | `sub rsp,0x28` already applied inside; the function is `__fastcall` and does not use `rbx`/`rsi`/`rdi`/`r12`–`r15`, so a `push`-based detour needs no extra shadow space beyond MSVC's 32-byte shadow store |
| Preserved registers | The trampoline only touches `rax`, `rcx`, `rdx`, `r8` and stack — safe to inline-hook with a 12-byte `mov rax, imm64; jmp rax` or a 5-byte `jmp rel32` into a code cave |
| Call frequency | At most once per tick, guarded by the `[manager+0x163C]` change cache; ≤ tick rate |
| Threads | Called from the game/tick thread only — the invoker has no locking, so the payload runs on whichever thread drives the tick |

Because the trampoline is tiny and does a real `call` (not a tail `jmp`) to `_Do_call`, a mod can either **detour `0x21C0`** (observe/rewrite the pair before it reaches the game's callback) or, more invasively, **replace the `std::function` in slot `g+0x108`** (`*(0x180241890)+0x108`) with its own, which is cleaner because the binder/`UnBindAllFB` machinery will manage it correctly.

### 7.3 Secondary hooks

| Effect | RVA | Args received | Notes |
|---|---|---|---|
| Viscosity | **`0x2200`** | `(int a, int b)` = `(sign ±1, magnitude)` | edge-triggered on `[manager+0x1644]` |
| Vibration | **`0x2240`** | `(int a, int b)` = `(clamped curve value, xmm7*xmm12)` | edge-triggered on `[manager+0x1640]` |
| Servo ON | **`0x2160`** | none | gated on *both* servo slots being set |
| Servo OFF | **`0x21A0`** | none | fires on race exit |
| Unbind all | **`0x2020`** | none | useful to detect teardown |
| FFB reset | **`0xC053`** (servo off) and **`0x6C7E`–`0x6D48`** (zero all effects) | — | detect race end |
| Per-tick gain | **`0xEFE7`** (call to the mega-function); read `[manager+0x4E4]` | the gain for this tick | ideal for *scaling* FFB without touching the callback |
| Lifecycle | **`0x5F030`** `?Start@…` | — | single init entry, for installing all hooks once |
| Host tick | **`0x5F2A0`** `?OnTick@…` | `float dt` (ignored by the callee) | zero internal callers ⇒ called by the EXE |

`0x5F030` and `0x5F2A0` are exported with stable mangled names, so a mod can `GetProcAddress` them rather than hard-code RVAs — far more robust across builds.

### 7.4 Risks and caveats

1. **Centering is dead code.** Neither `g+0x88` nor `g+0xC8` is ever invoked, and the EXE never binds them. Hooking either binder is pointless; there is no centering signal to observe.
2. **The mega-function is not hookable as a unit.** RVA `0x148A0`–`0x2D274` is 100,820 bytes; the only sane interception is the tiny invokers it calls.
3. **This build has no PDB and no RTTI for the FFB types**, so all names are inferred. RVAs are specific to this exact binary (`TimeDateStamp 0x69392E86`); a different build will shift them.
4. **`r13d` is not provably `+1`.** The forward dataflow could not be completed (3 unresolved indirect jump tables, incomplete CFG). Hook code should treat `direction` as a signed int and not assume it is only ever −1.
5. **Reordering/inlining risk is low for the invokers** (they are real, separately-addressable functions called with `call`, not inlined) but **high for anything inside the mega-function**.
6. **Multi-thread risk.** The trampolines perform no synchronisation and `UnBindAllFB` frees the `std::function` payloads. A hook that reads slot `_Ptr` while the game thread runs `UnBindAllFB` can observe a freed pointer. Read `_Ptr`, then validate, and prefer detouring the invoker over dereferencing the slot yourself.
7. **`Bind*` does not validate `this`** — the `rcx` (`this`) argument is ignored entirely, so calling the exported binders on any garbage `this` pointer works, which is convenient but also means the API gives no ownership signal.
8. **Timing is unproven** (§6): assume the frame rate, measure it, do not trust a constant.

---

## 8. Proven vs. inferred — summary

| Claim | Status |
|---|---|
| 123 exports, all `cUE4Interface`, 116 distinct RVAs, 6 aliases of stub `0x3DB0` | **PROVEN** |
| `cUE4Interface` is stateless (`this` ignored; ctor == copy-assign RVA `0x1480`) | **PROVEN** |
| All FFB callbacks live in the singleton `*(0x180241890)`, 7 slots at `+0x08/0x48/0x88/0xC8/0x108/0x148/0x188`, `_Ptr` at `+0x38`, `std::function` = `0x40` bytes | **PROVEN** (constructor at `0x5E084`–`0x5E0C3` + all seven binders + `UnBindAllFB`) |
| Slot writer mapping (which `Bind*` writes which slot) | **PROVEN** |
| Callback signatures `void(eTrqueDirection,int)`, `void(int,int)`, `void(int,int)`, `void()`, `void(int)` | **PROVEN** (mangled names + invoker argument marshalling) |
| Torque callback receives `(direction, abs(torque))` | **PROVEN** (`0x2BCC8` `abs`, `0x2BCCD` `mov edx,eax`, `0x2BCDB` call, and the `Torque %d` log at `0x2BCC6`–`0x2BD0E`) |
| `eTrqueDirection` is a 4-byte enum | **PROVEN** (`W4` in the mangled name) |
| `eTrqueDirection` has member `-1` | **PROVEN** (`0x2BCD1`, `0x6D19`) |
| `eTrqueDirection` has member `+1` | **INFERRED** (three `mov r13d,1` sites, intervening writes are pointer temporaries, `sign()` helper returns ±1; forward dataflow could not complete) |
| Enumerator *names* | **NOT RECOVERABLE** (no enum strings, no RTTI) |
| No centering callback is ever invoked or bound | **PROVEN** (no invoker for `g+0x88`/`g+0xC8`; zero `BindCentering` in the EXE) |
| No thread/timer inside IndRun.dll; `?OnTick` is host-driven | **PROVEN** (import list; zero internal callers of `0x5F2A0`) |
| `?Start@…` (`0x5F030`) is the single initialization entry | **PROVEN** (sole caller `0x5F09B` of the sole initializer `0x5DD30`) |
| Mega-function called only from `0xEFE7` (wrapper `0xEF15`–`0xEFFE`) | **PROVEN** (raw rel32 branch scan) |
| The game binds FFB by `GetProcAddress` on mangled names | **PROVEN** for the name list and dynamic-loading mechanism; the **exact call site** is NOT recoverable statically (0 xrefs to the names) |
| Update rate in ms | **NOT PROVABLE STATICALLY**; `timeBeginPeriod(1)` is proven, frame rate is not |
| Numeric FFB tuning constants and their addresses | **PROVEN** as values/addresses; their *semantic* role (which is a spring gain vs. a kickback scale) is **inferred** from the surrounding arithmetic |
| Runtime-tunable `.data` floats are config-loaded | **INFERRED** (one read site each, no writer inside the DLL, zero in the file image) |

---

## 9. Scratch tooling (all under `D:\INDAC\DGLab_IDAC\re\`)

| Script | Purpose |
|---|---|
| `p1_recon.py` → `recon1.json` | PE headers, sections, data directories, imports, exports |
| `eng2.py` | Function engine built on the EXCEPTION directory (`0x30B4` → 1039 functions spanning `0x1060`–`0x88866`). Subcommands `info`, `callers`, `refs`, `at`, `func` |
| `disp.py` | Raw 4-byte RIP-displacement scanner: for every byte offset `i`, checks `i+4+int32_at_i == target_rva`. Immune to capstone desynchronisation; the ground truth for cross-references |
| `xexe.py` | numpy sliding-window RIP-displacement scanner over the 69 MB EXE (`xrefstr`, `xrefva`, `find`) |
| `ida.py` | Basic linear disassembler with RIP annotation |
| `strs.py` | String finder + cross-references |
| `exports.py` | MSVC mangled-name demangler for this DLL's encodings; emits `_exports_table.md` and `_ffb_table.md` |
| `flow13.py` | Forward dataflow over the mega-function CFG tracking `r13d` (result: inconclusive — 3 unresolved indirect jump tables) |

**Lesson learned (important for anyone repeating this):** a naive linear `capstone` sweep of a 550 KB `.text` **silently desynchronises** and drops cross-references — an early sweep reported only 16 references to `0x241890` when there are 26, and reported "no xref" to the torque log string that demonstrably exists at `0x2BCF0`. Always drive per-function disassembly from the `.pdata`/EXCEPTION `RUNTIME_FUNCTION` table, and use raw displacement scanning for xrefs.

---

## Appendix A — Complete export inventory (123 ordinals)

| ord | RVA | mangled name | demangled |
|----:|-----|--------------|-----------|
| 1 | `0x001480` | `??0cUE4Interface@@QEAA@XZ` | `cUE4Interface::cUE4Interface()` |
| 3 | `0x001480` | `??4cUE4Interface@@QEAAAEAV0@AEBV0@@Z` | `cUE4Interface& cUE4Interface::operator=(cUE4Interface const&)` |
| 2 | `0x003db0` | `??1cUE4Interface@@QEAA@XZ` | `cUE4Interface::~cUE4Interface()` |
| 6 | `0x003db0` | `?BindBuntaChallenge@cUE4Interface@@QEAAXAEBV?$function@$$A6AXW4eBuntaChallenge@@@Z@std@@@Z` | `void cUE4Interface::BindBuntaChallenge(const std::function<void(?4, ?e, ?B, ?u, ?n, ?t, ?a, char, ?h, ?a, ?l, ?l, ?e, ?n, ?g, ?e, ?@, ?@)>&)` |
| 26 | `0x003db0` | `?DebugCarSpeedUp@cUE4Interface@@QEAAXM@Z` | `void cUE4Interface::DebugCarSpeedUp(float)` |
| 27 | `0x003db0` | `?DebugChangeMirrorAccessory@cUE4Interface@@QEAAXAEBUstMirrorAccessorySetting@@@Z` | `void cUE4Interface::DebugChangeMirrorAccessory(const stMirrorAccessorySetting&)` |
| 30 | `0x003db0` | `?DumpMemory@cUE4Interface@@SAXXZ` | `void cUE4Interface::DumpMemory(void)  [static]` |
| 85 | `0x003db0` | `?ResetRace@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ResetRace(void)` |
| 119 | `0x005aa0` | `?getDebugParamFlag@cUE4Interface@@SAPEAUValueFlag@@W4FLAG_ID@@@Z` | `ValueFlag* cUE4Interface::getDebugParamFlag(unsigned char, ?A, W4FLAG_ID::::ValueFlag&)  [static]` |
| 120 | `0x005aa0` | `?getDebugParamFloat@cUE4Interface@@SAPEAUValueFloat@@W4VALUE_ID@@@Z` | `ValueFloat* cUE4Interface::getDebugParamFloat(unsigned char, ?A, W4VALUE_ID::::ValueFloat&)  [static]` |
| 23 | `0x05ee50` | `?ChangeNewLog@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ChangeNewLog(void)` |
| 16 | `0x05ee60` | `?BindScreenPrintf@cUE4Interface@@QEAAXAEBV?$function@$$A6AXW4eColor@@PEBD@Z@std@@@Z` | `void cUE4Interface::BindScreenPrintf(const std::function<void(?4, ?e, char, ?o, ?l, ?o, ?r, ?@, ?@, const char*)>&)` |
| 82 | `0x05efc0` | `?ReloadConfig@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ReloadConfig(void)` |
| 123 | `0x05efd0` | `?setupTestModeSettingToConfig@cUE4Interface@@QEAAXAEBUstTestModeToConfig@@@Z` | `void cUE4Interface::setupTestModeSettingToConfig(const stTestModeToConfig&)` |
| 67 | `0x05efe0` | `?Pause@cUE4Interface@@QEAAX_N@Z` | `void cUE4Interface::Pause(bool)` |
| 107 | `0x05eff0` | `?TimerPause@cUE4Interface@@QEAAX_N@Z` | `void cUE4Interface::TimerPause(bool)` |
| 103 | `0x05f030` | `?Start@cUE4Interface@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@M@Z` | `void cUE4Interface::Start(const std::string&, ?V, ??, ?$, ?a, ?l, ?l, ?o, ?c, ?a, ?t, ?o, ?r, ?@, char, ?@, ?2, ?@, ?@, ?s, ?t, ?d, ?@, ?@, float)` |
| 90 | `0x05f160` | `?SetAppRootPath@cUE4Interface@@QEAAXPEBD@Z` | `void cUE4Interface::SetAppRootPath(const char*)` |
| 4 | `0x05f190` | `?AddCourseSetting@cUE4Interface@@QEAAXHAEBUstCourseSetting@@@Z` | `void cUE4Interface::AddCourseSetting(int, const stCourseSetting&)` |
| 32 | `0x05f1a0` | `?Exit@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::Exit(void)` |
| 66 | `0x05f2a0` | `?OnTick@cUE4Interface@@QEAAXM@Z` | `void cUE4Interface::OnTick(float)` |
| 24 | `0x05f370` | `?CreateGameSession@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::CreateGameSession(void)` |
| 28 | `0x05f380` | `?DestroyGameSession@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::DestroyGameSession(void)` |
| 25 | `0x05f3b0` | `?CreateRace@cUE4Interface@@QEAAXAEBUstRaceSetting@@@Z` | `void cUE4Interface::CreateRace(const stRaceSetting&)` |
| 31 | `0x05f3d0` | `?EntryActor@cUE4Interface@@QEAAXHAEBUstCharacterSetting@@@Z` | `void cUE4Interface::EntryActor(int, const stCharacterSetting&)` |
| 34 | `0x05f540` | `?FinishEntryActor@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::FinishEntryActor(void)` |
| 29 | `0x05f560` | `?DestroyRace@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::DestroyRace(void)` |
| 74 | `0x05f650` | `?ReadyRace@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ReadyRace(void)` |
| 104 | `0x05f6d0` | `?StartRace@cUE4Interface@@QEAAXH_N@Z` | `void cUE4Interface::StartRace(int, bool)` |
| 88 | `0x05f750` | `?RetireRace@cUE4Interface@@QEAAXW4eRetireRaceType@@@Z` | `void cUE4Interface::RetireRace(eRetireRaceType)` |
| 95 | `0x05f7a0` | `?SetRaceOffsetTime@cUE4Interface@@QEAAXH@Z` | `void cUE4Interface::SetRaceOffsetTime(int)` |
| 52 | `0x05f7d0` | `?GetRaceTime@cUE4Interface@@QEBAHXZ` | `void cUE4Interface::GetRaceTime(void) const` |
| 58 | `0x05f820` | `?GetSyncedRaceTime@cUE4Interface@@QEBAHXZ` | `void cUE4Interface::GetSyncedRaceTime(void) const` |
| 55 | `0x05f860` | `?GetStartRaceTime@cUE4Interface@@QEBAHXZ` | `void cUE4Interface::GetStartRaceTime(void) const` |
| 54 | `0x05f890` | `?GetRestartWaitEndRaceTime@cUE4Interface@@QEBAHXZ` | `void cUE4Interface::GetRestartWaitEndRaceTime(void) const` |
| 59 | `0x05f8c0` | `?IsRestartRace@cUE4Interface@@QEBA_NXZ` | `void cUE4Interface::IsRestartRace(double, void) const` |
| 89 | `0x05f8f0` | `?SetAccel@cUE4Interface@@QEAAXE@Z` | `void cUE4Interface::SetAccel(unsigned char)` |
| 91 | `0x05f920` | `?SetBrake@cUE4Interface@@QEAAXE@Z` | `void cUE4Interface::SetBrake(unsigned char)` |
| 99 | `0x05f950` | `?SetSteer@cUE4Interface@@QEAAXC@Z` | `void cUE4Interface::SetSteer(char)` |
| 97 | `0x05f970` | `?SetShift@cUE4Interface@@QEAAXC@Z` | `void cUE4Interface::SetShift(char)` |
| 108 | `0x05f980` | `?TrggierAutoRun@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::TrggierAutoRun(void)` |
| 72 | `0x05f9a0` | `?PressUp@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::PressUp(void)` |
| 68 | `0x05f9b0` | `?PressDown@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::PressDown(void)` |
| 69 | `0x05f9c0` | `?PressLeft@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::PressLeft(void)` |
| 70 | `0x05f9d0` | `?PressRight@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::PressRight(void)` |
| 71 | `0x05f9e0` | `?PressStart@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::PressStart(void)` |
| 73 | `0x05f9f0` | `?PressView@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::PressView(void)` |
| 80 | `0x05fa00` | `?ReleaseUp@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ReleaseUp(void)` |
| 76 | `0x05fa10` | `?ReleaseDown@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ReleaseDown(void)` |
| 77 | `0x05fa20` | `?ReleaseLeft@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ReleaseLeft(void)` |
| 78 | `0x05fa30` | `?ReleaseRight@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ReleaseRight(void)` |
| 79 | `0x05fa40` | `?ReleaseStart@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ReleaseStart(void)` |
| 81 | `0x05fa50` | `?ReleaseView@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ReleaseView(void)` |
| 110 | `0x05fa60` | `?UnBindAllFB@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::UnBindAllFB(void)` |
| 18 | `0x05fa70` | `?BindServoOn@cUE4Interface@@QEAAXV?$function@$$A6AXXZ@std@@@Z` | `void cUE4Interface::BindServoOn(std::function<void(void)>&&)` |
| 17 | `0x05fb10` | `?BindServoOff@cUE4Interface@@QEAAXV?$function@$$A6AXXZ@std@@@Z` | `void cUE4Interface::BindServoOff(std::function<void(void)>&&)` |
| 8 | `0x05fbb0` | `?BindCentering@cUE4Interface@@QEAAXV?$function@$$A6AXXZ@std@@@Z` | `void cUE4Interface::BindCentering(std::function<void(void)>&&)` |
| 7 | `0x05fc60` | `?BindCentering@cUE4Interface@@QEAAXV?$function@$$A6AXH@Z@std@@@Z` | `void cUE4Interface::BindCentering(std::function<void(int)>&&)` |
| 19 | `0x05fd10` | `?BindTorqueFunction@cUE4Interface@@QEAAXV?$function@$$A6AXW4eTrqueDirection@nsFeedbackDefine@@H@Z@std@@@Z` | `void cUE4Interface::BindTorqueFunction(std::function<void(nsFeedbackDefine::eTrqueDirection, int)>&&)` |
| 22 | `0x05fd80` | `?BindViscosityFunction@cUE4Interface@@QEAAXV?$function@$$A6AXHH@Z@std@@@Z` | `void cUE4Interface::BindViscosityFunction(std::function<void(int, int)>&&)` |
| 21 | `0x05fe30` | `?BindVibrationFunction@cUE4Interface@@QEAAXV?$function@$$A6AXHH@Z@std@@@Z` | `void cUE4Interface::BindVibrationFunction(std::function<void(int, int)>&&)` |
| 15 | `0x05fee0` | `?BindRaceTimerUpdater@cUE4Interface@@QEAAXAEBV?$function@$$A6AXAEAH@Z@std@@@Z` | `void cUE4Interface::BindRaceTimerUpdater(const std::function<void(unsigned char, ?A, int)>&)` |
| 14 | `0x060050` | `?BindRaceInfoUpdater@cUE4Interface@@QEAAXAEBV?$function@$$A6AXAEBUstRaceInfo@@@Z@std@@@Z` | `void cUE4Interface::BindRaceInfoUpdater(const std::function<void(unsigned char, ?B, stRaceInfo&)>&)` |
| 13 | `0x0601c0` | `?BindRaceGoalEvent@cUE4Interface@@QEAAXAEBV?$function@$$A6AXH@Z@std@@@Z` | `void cUE4Interface::BindRaceGoalEvent(const std::function<void()>&)` |
| 12 | `0x0601f0` | `?BindRaceFinishEvent@cUE4Interface@@QEAAXAEBV?$function@$$A6AXAEBUstRaceExitState@@@Z@std@@@Z` | `void cUE4Interface::BindRaceFinishEvent(const std::function<void(unsigned char, ?B, stRaceExitState&)>&)` |
| 114 | `0x060360` | `?bindEffectNotifer@cUE4Interface@@QEAAXAEBV?$function@$$A6AXAEBVcEffectState@@@Z@std@@@Z` | `void cUE4Interface::bindEffectNotifer(const std::function<void(unsigned char, ?B, ?V, ?c, unsigned char, ?f, ?f, ?e, ?c, ?t, ?S, ?t, ?a, ?t, ?e, ?@, ?@)>&)` |
| 113 | `0x060390` | `?bindBGEffectNotifer@cUE4Interface@@QEAAXAEBV?$function@$$A6AXHW4enAvaterBGEffect@@@Z@std@@@Z` | `void cUE4Interface::bindBGEffectNotifer(const std::function<void(enAvaterBGEffect)>&)` |
| 115 | `0x0603c0` | `?bindFacialNotifer@cUE4Interface@@QEAAXAEBV?$function@$$A6AXHW4enAvaterFacial@@@Z@std@@@Z` | `void cUE4Interface::bindFacialNotifer(const std::function<void(enAvaterFacial)>&)` |
| 118 | `0x0603f0` | `?bindRaceSuspendedNotifer@cUE4Interface@@QEAAXAEBV?$function@$$A6AXXZ@std@@@Z` | `void cUE4Interface::bindRaceSuspendedNotifer(const std::function<void()>&)` |
| 117 | `0x060420` | `?bindPreSaveGhost@cUE4Interface@@QEAAXAEBV?$function@$$A6A_NHHH@Z@std@@@Z` | `void cUE4Interface::bindPreSaveGhost(const std::function<void(int, int, int)>&)` |
| 116 | `0x060590` | `?bindPostSaveGhost@cUE4Interface@@QEAAXAEBV?$function@$$A6AXHHHPEBD@Z@std@@@Z` | `void cUE4Interface::bindPostSaveGhost(const std::function<void(int, int, const char*)>&)` |
| 111 | `0x060700` | `?UnBindRace@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::UnBindRace(void)` |
| 51 | `0x060720` | `?GetRaceActorNum@cUE4Interface@@QEBAHXZ` | `void cUE4Interface::GetRaceActorNum(void) const` |
| 44 | `0x060750` | `?GetLocalActorID@cUE4Interface@@QEBAHXZ` | `void cUE4Interface::GetLocalActorID(void) const` |
| 37 | `0x060780` | `?GetEntryID@cUE4Interface@@QEBAHH@Z` | `void cUE4Interface::GetEntryID(int) const` |
| 41 | `0x0607b0` | `?GetLastTime@cUE4Interface@@QEBAIH@Z` | `void cUE4Interface::GetLastTime(int) const` |
| 47 | `0x060810` | `?GetPacketLossCount@cUE4Interface@@QEBAIH@Z` | `void cUE4Interface::GetPacketLossCount(int) const` |
| 48 | `0x060880` | `?GetPacketLossMax@cUE4Interface@@QEBAIH@Z` | `void cUE4Interface::GetPacketLossMax(int) const` |
| 5 | `0x060950` | `?BindActorCarState@cUE4Interface@@QEAAXHV?$function@$$A6AXAEBUstCarStateInfo@@@Z@std@@@Z` | `void cUE4Interface::BindActorCarState(int, std::function<void(const stCarStateInfo&)>&&)` |
| 9 | `0x060b80` | `?BindCheckPointPassed@cUE4Interface@@QEAAXHAEBV?$function@$$A6AXHHH@Z@std@@@Z` | `void cUE4Interface::BindCheckPointPassed(int, const std::function<void(int, int)>&)` |
| 101 | `0x060d20` | `?SetupEngineSound@cUE4Interface@@QEAAXHPEAVEngineSoundBridgeBase@@AEBV?$function@$$A6AXH_NH0H0@Z@std@@@Z` | `void cUE4Interface::SetupEngineSound(int, void*, ?V, unsigned char, ?n, ?g, ?i, ?n, ?e, ?S, ?o, ?u, ?n, ?d, ?B, ?r, ?i, ?d, ?g, ?e, ?B, ?a, ?s, ?e, ?@, ?@, const std::function<void(bool, int, ?0, int, ?0)>&)` |
| 20 | `0x060e00` | `?BindUDPSender@cUE4Interface@@QEAAXHAEBV?$function@$$A6AXPEAXH@Z@std@@@Z` | `void cUE4Interface::BindUDPSender(int, const std::function<void(unsigned char, ?A, void, int)>&)` |
| 11 | `0x060f20` | `?BindOnHitOtherCar@cUE4Interface@@QEAAXHAEBV?$function@$$A6AXAEBUstOnHitOtherCar@@@Z@std@@@Z` | `void cUE4Interface::BindOnHitOtherCar(int, const std::function<void(unsigned char, ?B, stOnHitOtherCar&)>&)` |
| 109 | `0x0610c0` | `?UnBindActorEvent@cUE4Interface@@QEAAXH@Z` | `void cUE4Interface::UnBindActorEvent(int)` |
| 75 | `0x0611d0` | `?Recieve@cUE4Interface@@QEAAXHPEAXH@Z` | `void cUE4Interface::Recieve(int, void*, void, int)` |
| 93 | `0x061240` | `?SetHeadLight@cUE4Interface@@QEAAXH_N@Z` | `void cUE4Interface::SetHeadLight(int, bool)` |
| 92 | `0x0612b0` | `?SetHasRetractableLight@cUE4Interface@@QEAAXH_N@Z` | `void cUE4Interface::SetHasRetractableLight(int, bool)` |
| 96 | `0x061320` | `?SetRetractableLightMoving@cUE4Interface@@QEAAXH_N@Z` | `void cUE4Interface::SetRetractableLightMoving(int, bool)` |
| 60 | `0x061390` | `?IsRetractableLightMoving@cUE4Interface@@QEAA_NH@Z` | `void cUE4Interface::IsRetractableLightMoving(double, int)` |
| 65 | `0x061400` | `?NoticeDriverView@cUE4Interface@@QEAAX_N@Z` | `void cUE4Interface::NoticeDriverView(bool)` |
| 10 | `0x061430` | `?BindGetSoundUE4Func@cUE4Interface@@QEAAXAEBV?$function@$$A6APEAVcSoundUE4@@XZ@std@@@Z` | `void cUE4Interface::BindGetSoundUE4Func(const std::function<void(?A, ?V, ?c, ?S, ?o, ?u, ?n, ?d, E4&, void)>&)` |
| 98 | `0x0615a0` | `?SetSoundVolumes@cUE4Interface@@QEAAXAEBUstVolumeProfile@nsAppAudio@@@Z` | `void cUE4Interface::SetSoundVolumes(const nsAppAudio::stVolumeProfile&)` |
| 94 | `0x0615d0` | `?SetRaceBrake@cUE4Interface@@QEAAX_N@Z` | `void cUE4Interface::SetRaceBrake(bool)` |
| 33 | `0x061600` | `?FindCarStateInfo@cUE4Interface@@SA?AUstCarStateInfo@@H@Z` | `void cUE4Interface::FindCarStateInfo(?A, stCarStateInfo&, int)  [static]` |
| 45 | `0x061b00` | `?GetMatchingMax@cUE4Interface@@SAHXZ` | `int cUE4Interface::GetMatchingMax(void)  [static]` |
| 56 | `0x061b10` | `?GetSteeringAdjustment@cUE4Interface@@SAHXZ` | `int cUE4Interface::GetSteeringAdjustment(void)  [static]` |
| 64 | `0x061b20` | `?LoadPathPoint@cUE4Interface@@QEAA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z` | `void cUE4Interface::LoadPathPoint(double, const std::string&, ?V, ??, ?$, ?a, ?l, ?l, ?o, ?c, ?a, ?t, ?o, ?r, ?@, char, ?@, ?2, ?@, ?@, ?s, ?t, ?d, ?@, ?@)` |
| 112 | `0x061c00` | `?UnLoadPathPoint@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::UnLoadPathPoint(void)` |
| 50 | `0x061c30` | `?GetPathPointsNum@cUE4Interface@@QEBAHXZ` | `void cUE4Interface::GetPathPointsNum(void) const` |
| 49 | `0x061c40` | `?GetPathPoint@cUE4Interface@@QEBA?AUfloat3@@H@Z` | `void cUE4Interface::GetPathPoint(??, ?A, float3&, int) const` |
| 102 | `0x061cf0` | `?SetupGhostResource@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::SetupGhostResource(void)` |
| 62 | `0x061df0` | `?LoadGhostResource@cUE4Interface@@QEAA_NPEBD@Z` | `void cUE4Interface::LoadGhostResource(double, const char*)` |
| 61 | `0x061e60` | `?LoadGhostResource@cUE4Interface@@QEAA_NHW4eCourseDir@@W4eCourseWeather@@W4eCourseDay@@E@Z` | `void cUE4Interface::LoadGhostResource(double, int, W4eCourseDay::::W4eCourseWeather::::eCourseDir, unsigned char)` |
| 84 | `0x061e80` | `?ResetGhostResource@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::ResetGhostResource(void)` |
| 39 | `0x061ee0` | `?GetGhostData@cUE4Interface@@QEAA_NAEAUstGhostData@@@Z` | `void cUE4Interface::GetGhostData(double, ?A, unsigned char, ?A, stGhostData&)` |
| 38 | `0x061fa0` | `?GetGhostCarData@cUE4Interface@@QEAA_NAEAUstGhostDataCar@@H@Z` | `void cUE4Interface::GetGhostCarData(double, ?A, unsigned char, ?A, stGhostDataCar&, int)` |
| 40 | `0x062160` | `?GetGhostGoalTime@cUE4Interface@@QEAAHH@Z` | `void cUE4Interface::GetGhostGoalTime(int)` |
| 53 | `0x062200` | `?GetReplayStockPath@cUE4Interface@@QEAAXPEADH@Z` | `void cUE4Interface::GetReplayStockPath(void*, char, int)` |
| 43 | `0x062250` | `?GetLineGuideGhostRecordNum@cUE4Interface@@QEAAHH@Z` | `void cUE4Interface::GetLineGuideGhostRecordNum(int)` |
| 42 | `0x062280` | `?GetLineGuideGhostRecordData@cUE4Interface@@QEAA_NAEAPEAUstGhostRecordData@@HHHH@Z` | `void cUE4Interface::GetLineGuideGhostRecordData(double, ?A, unsigned char, ?A, const stGhostRecordData*, int, int, int, int)` |
| 63 | `0x062290` | `?LoadLineGuideResource@cUE4Interface@@QEAA_NPEBD@Z` | `void cUE4Interface::LoadLineGuideResource(double, const char*)` |
| 105 | `0x062330` | `?StartRollingStart@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::StartRollingStart(void)` |
| 106 | `0x062360` | `?SuspendRace@cUE4Interface@@QEAAXMM@Z` | `void cUE4Interface::SuspendRace(float, float)` |
| 87 | `0x062440` | `?RestartRaceAutorun@cUE4Interface@@QEAAXXZ` | `void cUE4Interface::RestartRaceAutorun(void)` |
| 86 | `0x0625a0` | `?RestartRace@cUE4Interface@@QEAAXM@Z` | `void cUE4Interface::RestartRace(float)` |
| 100 | `0x062610` | `?SetStoryRaceNo@cUE4Interface@@QEAAXH@Z` | `void cUE4Interface::SetStoryRaceNo(int)` |
| 57 | `0x062640` | `?GetStoryRollingStartPosition@cUE4Interface@@SA_NHAEAUfloat3@@0@Z` | `void cUE4Interface::GetStoryRollingStartPosition(double, int, ?A, unsigned char, ?A, float3&, ?0)  [static]` |
| 83 | `0x062e40` | `?RequestStartEngineSound@cUE4Interface@@SAXXZ` | `void cUE4Interface::RequestStartEngineSound(void)  [static]` |
| 46 | `0x062e70` | `?GetMirrorAccessoryBoneMatrix@cUE4Interface@@QEAAPEBUfloat4x4@@AEAH@Z` | `void cUE4Interface::GetMirrorAccessoryBoneMatrix(const char*, float4x4&, ?A, unsigned char, ?A, int)` |
| 36 | `0x062eb0` | `?GetDroneInfo@cUE4Interface@@QEAAPEBUstDroneSetting@@XZ` | `void cUE4Interface::GetDroneInfo(const char*, stDroneSetting&, void)` |
| 35 | `0x062ee0` | `?GetCarScale@cUE4Interface@@SAMXZ` | `float cUE4Interface::GetCarScale(void)  [static]` |
| 122 | `0x062ef0` | `?isLiveDebugParam@cUE4Interface@@SA_NXZ` | `void cUE4Interface::isLiveDebugParam(double, void)  [static]` |
| 121 | `0x062f00` | `?getIndRunPlaylogValue@cUE4Interface@@QEAAHW4kINDRUN_PLAYLOG_VALUE@@@Z` | `void cUE4Interface::getIndRunPlaylogValue(kINDRUN_PLAYLOG_VALUE)` |

