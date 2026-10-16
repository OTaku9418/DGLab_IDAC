# IndRun.dll — FFB channel trampolines: hook mechanics

Target: `D:\INDAC\MODAC321\WindowsNoEditor\GameProject\Plugins\IndRun\Binaries\Win64\IndRun.dll`
(2,634,240 bytes, MSVC x64, 123 mangled C++ exports)

All RVAs are relative to the PE `OptionalHeader.ImageBase`, read out of the header as:

```
ImageBase                 = 0x0000000180000000
AddressOfEntryPoint (RVA) = 0x00086BE8
SizeOfImage               = 0x028A0000
Machine                   = 0x8664
DllCharacteristics        = 0x0060   (DYNAMIC_BASE | HIGH_ENTROPY_VA)
```

Absolute address = `RVA + 0x180000000` **plus the runtime relocation delta** (`DYNAMIC_BASE` is set, so
the module is rebased by the loader; never hard-code `0x180000000` in a hook — use
`GetModuleHandle`/the `HMODULE` you injected with and add the RVA).

Relevant sections (VA RVA, VirtualSize, SizeOfRawData):

| section | RVA | vsize | rawsize |
|---|---|---|---|
| `.text`   | 0x0001000 | 0x0088000 | 0x0087A00 |
| `.rdata`  | 0x0089000 | 0x01B2000 | 0x01B1600 |
| `.data`   | 0x023B000 | 0x0007000 | 0x0002800 |
| `.pdata`  | 0x0242000 | 0x0045000 | 0x0045000 |
| `.rsrc`   | 0x0287000 | — | — |
| `.reloc`  | 0x0288000 | — | — |
| `.SCY`    | 0x0289000 | — | — |

`0x241890` therefore lies in `.data` → absolute **0x180241890**.

---

## 0. The singleton and the seven `std::function` slots (answers Q3 and Q6)

### 0.1 `0x180241890` is a pointer variable, not the object

Every consumer loads it with a **single** RIP-relative dereference:

```
48 8b 0d c3 f6 23 00    mov rcx, qword ptr [rip + 0x23f6c3]   ; == [0x180241890]
```

so at runtime the singleton pointer is

```c
void* g = *reinterpret_cast<void**>(moduleBase + 0x241890);   // cUE4Interface*
```

There is **no** second dereference anywhere, and the object returned by that first dereference has a
**vtable pointer at its offset `+0x00`** (see 0.2), which is exactly why the first `std::function`
slot starts at `+0x08` and not at `+0x00`. Use `sizeof(void*)` reads only; do not cache the value
across frames — it is created lazily and can still be null.

### 0.2 Creation of the singleton (RVA 0x5E055 – 0x5E0C3)

```asm
18005E055  48 8b d5                 mov     rdx, rbp                     ; rdx = 0
18005E058  48 39 2d 39 38 1e 00     cmp     qword ptr [rip + 0x1e3839], rbp   ; [0x180241898] == 0 ?
18005E05F  75 07                    jne     0x18005E068
18005E061  48 89 15 30 38 1e 00     mov     qword ptr [rip + 0x1e3830], rdx   ; [0x180241898] = 0
18005E068  ba 08 00 00 00           mov     edx, 8
18005E06D  b9 c8 01 00 00           mov     ecx, 0x1C8                   ; 456 bytes
18005E072  ff 15 58 b1 02 00        call    qword ptr [rip + 0x2b158]    ; operator new(0x1C8, 8)
18005E078  48 85 c0                 test    rax, rax
18005E07B  74 3a                    je      0x18005E0B7                  ; alloc failed -> rax = 0
18005E07D  48 8d 0d 74 1d 1b 00     lea     rcx, [rip + 0x1b1d74]        ; vtable of cUE4Interface
18005E084  48 89 08                 mov     qword ptr [rax], rcx         ; vptr at +0x00
18005E087  48 89 68 40              mov     qword ptr [rax + 0x40], rbp  ; slot+0x08 std::function : _Ptr = 0
18005E08B  48 89 a8 80 00 00 00     mov     qword ptr [rax + 0x80], rbp  ; slot+0x48 : _Ptr = 0
18005E092  48 89 a8 c0 00 00 00     mov     qword ptr [rax + 0xc0], rbp  ; slot+0x88 : _Ptr = 0
18005E099  48 89 a8 00 01 00 00     mov     qword ptr [rax + 0x100], rbp ; slot+0xc8 : _Ptr = 0
18005E0A0  48 89 a8 40 01 00 00     mov     qword ptr [rax + 0x140], rbp ; slot+0x108: _Ptr = 0
18005E0A7  48 89 a8 80 01 00 00     mov     qword ptr [rax + 0x180], rbp ; slot+0x148: _Ptr = 0
18005E0AE  48 89 a8 c0 01 00 00     mov     qword ptr [rax + 0x1c0], rbp ; slot+0x188: _Ptr = 0
18005E0B5  eb 0c                    jmp     0x18005E0C3
18005E0B7  48 8b c5                 mov     rax, rbp                     ; failure path: rax = 0
18005E0BA  48 39 2d cf 37 1e 00     cmp     qword ptr [rip + 0x1e37cf], rbp   ; [0x180241890] == 0 ?
18005E0C1  75 07                    jne     0x18005E0CA
18005E0C3  48 89 05 c6 37 1e 00     mov     qword ptr [rip + 0x1e37c6], rax   ; [0x180241890] = object
18005E0CA  ba 08 00 00 00           mov     edx, 8
18005E0CF  b9 98 00 00 00           mov     ecx, 0x98
18005E0D4  ff 15 f6 b0 02 00        call    qword ptr [rip + 0x2b0f6]    ; operator new(0x98, 8) — a sub-object
```

RIP arithmetic check for the two stores that matter:

* `0x18005E0C3 + 7 + 0x1E37C6 = 0x180241890` — the singleton store.
* `0x18005E0BA + 7 + 0x1E37CF = 0x180241890` — the double-check guard.

Three conclusions that all confirm the seven-slot layout independently:

1. The object is allocated as **`operator new(0x1C8, 8)` = 456 bytes**, and the last slot ends exactly
   at `0x188 + 0x40 = 0x1C8`. The object is a vptr plus exactly seven `0x40`-byte `std::function`s —
   nothing else fits.
2. The constructor zeroes exactly seven fields: `+0x40, +0x80, +0xC0, +0x100, +0x140, +0x180,
   +0x1C0`. Those are `slot_offset + 0x38` for slots `+0x08, +0x48, +0x88, +0xC8, +0x108, +0x148,
   +0x188` — i.e. exactly the seven `_Ptr` fields. This also pins `_Ptr` at `std::function + 0x38`.
3. `UnBindAllFB` (section 6) tears down exactly those same seven offsets.

### 0.3 Slot map (re-verified from the *binding* functions)

Each `Bind*` export reads `[0x241890]`, adds a constant, and passes the destination to a
`std::function` move-assign helper (`0x180002400` for six of them, `0x180002280` for torque):

| singleton offset | `_Ptr` offset | binder export (RVA) | `add rcx, imm` site | bound type |
|---|---|---|---|---|
| `+0x08`  | `+0x40`  | `?BindServoOn@cUE4Interface@@QEAAXV?$function@$$A6AXXZ@std@@@Z` (0x5FA70)   | `0x5FAC2` `add rcx,8`     | `void(void)` |
| `+0x48`  | `+0x80`  | `?BindServoOff@cUE4Interface@@QEAAXV?$function@$$A6AXXZ@std@@@Z` (0x5FB10)  | `0x5FB62` `add rcx,0x48`  | `void(void)` |
| `+0x88`  | `+0xC0`  | `?BindCentering@cUE4Interface@@QEAAXV?$function@$$A6AXXZ@std@@@Z` (0x5FBB0) | `0x5FC02` `add rcx,0x88`  | `void(void)` |
| `+0xC8`  | `+0x100` | `?BindCentering@cUE4Interface@@QEAAXV?$function@$$A6AXH@Z@std@@@Z` (0x5FC60) | `0x5FCB2` `add rcx,0xC8` | `void(int)` |
| `+0x108` | `+0x140` | `?BindTorqueFunction@cUE4Interface@@QEAAXV?$function@$$A6AXW4eTrqueDirection@nsFeedbackDefine@@H@Z@std@@@Z` (0x5FD10) | inside `0x180002280`: `add rbx,0x108` | `void(eTrqueDirection,int)` |
| `+0x148` | `+0x180` | `?BindViscosityFunction@cUE4Interface@@QEAAXV?$function@$$A6AXHH@Z@std@@@Z` (0x5FD80) | `0x5FDD2` `add rcx,0x148` | `void(int,int)` |
| `+0x188` | `+0x1C0` | `?BindVibrationFunction@cUE4Interface@@QEAAXV?$function@$$A6AXHH@Z@std@@@Z` (0x5FE30) | `0x5FE82` `add rcx,0x188` | `void(int,int)` |

The mapping given in the task is **confirmed exactly**. Note that "centering" is genuinely two slots
(two overloads: `BindCentering(std::function<void()>)` and `BindCentering(std::function<void(int)>)`),
so the seven slots are: servo-on, servo-off, centering-A, centering-B, torque, viscosity, vibration.

MSVC closure-object vtable layout observed on the object at `_Ptr` (identical for every slot):

| vtable byte offset | slot | meaning |
|---|---|---|
| `+0x00` | 0 | `_Copy(void*)` |
| `+0x08` | 1 | `_Move(void*)` |
| `+0x10` | 2 | `_Do_call` — the actual invocation |
| `+0x20` | 4 | `_Delete_this(bool)` — used by `UnBindAllFB` |

All five invoke trampolines use slot index 2 (`[rax+0x10]`); teardown uses slot index 4 (`[rax+0x20]`).

---

## 1. Q1 — the five invoke trampolines, full listings

Every listing below is linear-disassembled from the function entry (which is a verified instruction
boundary; the identical bytes were also obtained by a whole-`.text` sweep). `.pdata` boundaries,
unwind records and the exact prologue byte counts are given per function, because they decide how
many bytes MinHook has to relocate.

### 1.1 servo-on — RVA `0x2160` (abs `0x180002160`), 0x2160–0x2199, len 0x3A

`.pdata`: (0x2160, 0x219A), `UNWIND_INFO` at RVA `0x230F90` = `01 04 01 00 04 42 00 00`
(version 1, no handler, prologsize 4, one code: `UWOP_ALLOC_SMALL` size `4*8+8 = 0x28`).

```asm
180002160  48 83 ec 28                 sub     rsp, 0x28                     ; prologue: 4 bytes
180002164  48 8b 05 25 f7 23 00        mov     rax, [rip + 0x23f725]         ; rax = *(0x180241890) = g
18000216B  48 85 c0                    test    rax, rax
18000216E  74 25                       je      0x180002195                   ; if (!g) return
180002170  48 83 b8 80 00 00 00 00     cmp     qword ptr [rax + 0x80], 0     ; g->slot48._Ptr == 0 ?
180002178  74 1b                       je      0x180002195                   ; if (!g->slot48) return
18000217A  48 8b 48 40                 mov     rcx, qword ptr [rax + 0x40]   ; rcx = g->slot08._Ptr
18000217E  48 85 c9                    test    rcx, rcx
180002181  75 07                       jne     0x18000218a
180002183  ff 15 6f 6f 08 00           call    qword ptr [rip + 0x86f6f]     ; IAT 0x1800890F8
180002189  cc                          int3
18000218A  48 8b 01                    mov     rax, qword ptr [rcx]          ; closure vtable
18000218D  48 83 c4 28                 add     rsp, 0x28
180002191  48 ff 60 10                 jmp     qword ptr [rax + 0x10]        ; tail-jump to _Do_call
180002195  48 83 c4 28                 add     rsp, 0x28
180002199  c3                          ret
```

* Stack frame: **yes**, `sub rsp,0x28` (32-byte shadow space + 8 alignment). No pushes, no saved
  non-volatiles.
* Singleton load: `mov rax,[rip+0x23f725]` (7 bytes), single dereference of `0x180241890`.
* Null checks: `g` tested at `0x216B`; then the gate is `[g+0x80]` (slot `+0x48`); the invoked
  target `[g+0x40]` (slot `+0x08`) is checked last.
* Call through vtable: **tail jump** `jmp qword ptr [rax+0x10]` (vtable slot 2), after the frame has
  been released. No return address of its own is on the stack at that point.
* IAT `0x1800890F8` resolves to `msvcp140.dll!?_Xbad_function_call@std@@YAXXZ` — the standard
  "call on an empty `std::function`" throw.
* **Behavioural quirk that matters for a hook:** servo-on is *gated on the servo-off slot being
  non-empty* (`cmp [rax+0x80],0`) but *invokes the servo-on slot* (`[rax+0x40]`). So the observable
  semantics are `if (g && g->slot48) g->slot08();` and `g->slot08()` throws if it is unbound. A
  detour of `0x2160` inherits this: it will not be entered at all unless servo-off has been bound.
* **ABI at entry: `void(void)`.** No argument register is read or modified. `ecx/edx/r8/r9` are
  dead on entry and are not forwarded (they are not even preserved).

### 1.2 servo-off — RVA `0x21A0` (abs `0x1800021A0`), 0x21A0–0x21BF, len 0x20, leaf

`.pdata`: **no entry at all** (the function has no unwind record; it is a frameless leaf, so the
linker emitted nothing). Treat `0x21A0` as an entry point only because the single call site targets
it.

```asm
1800021A0  48 8b 05 e9 f6 23 00        mov     rax, [rip + 0x23f6e9]         ; rax = g
1800021A7  48 85 c0                    test    rax, rax
1800021AA  74 13                       je      0x1800021bf                   ; if (!g) return
1800021AC  48 8b 88 80 00 00 00        mov     rcx, qword ptr [rax + 0x80]   ; rcx = g->slot48._Ptr
1800021B3  48 85 c9                    test    rcx, rcx
1800021B6  74 07                       je      0x1800021bf                   ; if (!slot48) return
1800021B8  48 8b 01                    mov     rax, qword ptr [rcx]
1800021BB  48 ff 60 10                 jmp     qword ptr [rax + 0x10]        ; tail-jump to _Do_call
1800021BF  c3                          ret
```

* Stack frame: **none**. No `sub rsp`, no pushes, no non-volatile saves. Purely a tail jump.
* Note there is **no** `bad_function_call` path here: the null check on `[g+0x80]` *is* the check on
  the invoked target, so the invoke is always on a non-null closure.
* **ABI at entry: `void(void)`.**

### 1.3 torque — RVA `0x21C0` (abs `0x1800021C0`), 0x21C0–0x21FA, len 0x3B

`.pdata`: (0x21C0, 0x21FB), `UNWIND_INFO` at RVA `0x231170` = `01 04 01 00 04 62 00 00`
(prologsize 4, one code: `UWOP_ALLOC_SMALL` size `6*8+8 = 0x38`). The unwind record at `0x231170` is
**shared** by 0x21C0, 0x2200 and 0x2240 (folded identical unwind info), which is itself evidence that
the three bodies are structurally identical.

```asm
1800021C0  48 83 ec 38                 sub     rsp, 0x38                     ; prologue: 4 bytes
1800021C4  8b c1                       mov     eax, ecx                      ; eax = arg1 = DIRECTION
1800021C6  48 8b 0d c3 f6 23 00        mov     rcx, [rip + 0x23f6c3]         ; rcx = g
1800021CD  48 85 c9                    test    rcx, rcx
1800021D0  74 24                       je      0x1800021f6                   ; if (!g) return
1800021D2  48 8b 89 40 01 00 00        mov     rcx, qword ptr [rcx + 0x140]  ; rcx = g->slot108._Ptr
1800021D9  48 85 c9                    test    rcx, rcx
1800021DC  74 18                       je      0x1800021f6                   ; if (!slot108) return
1800021DE  89 54 24 20                 mov     dword ptr [rsp + 0x20], edx   ; [rsp+0x20] = arg2 = MAGNITUDE
1800021E2  4c 8d 44 24 20              lea     r8, [rsp + 0x20]              ; r8 = &MAGNITUDE
1800021E7  89 44 24 28                 mov     dword ptr [rsp + 0x28], eax   ; [rsp+0x28] = arg1 = DIRECTION
1800021EB  48 8d 54 24 28              lea     rdx, [rsp + 0x28]             ; rdx = &DIRECTION
1800021F0  48 8b 01                    mov     rax, qword ptr [rcx]
1800021F3  ff 50 10                    call    qword ptr [rax + 0x10]        ; _Do_call
1800021F6  48 83 c4 38                 add     rsp, 0x38
1800021FA  c3                          ret
```

### 1.4 viscosity — RVA `0x2200` (abs `0x180002200`), 0x2200–0x223A, len 0x3B

Byte-for-byte identical to torque except for two immediates:

```asm
180002200  48 83 ec 38                 sub     rsp, 0x38
180002204  8b c1                       mov     eax, ecx                      ; eax = arg1
180002206  48 8b 0d 83 f6 23 00        mov     rcx, [rip + 0x23f683]         ; rcx = g
18000220D  48 85 c9                    test    rcx, rcx
180002210  74 24                       je      0x180002236
180002212  48 8b 89 80 01 00 00        mov     rcx, qword ptr [rcx + 0x180]  ; g->slot148._Ptr
180002219  48 85 c9                    test    rcx, rcx
18000221C  74 18                       je      0x180002236
18000221E  89 54 24 20                 mov     dword ptr [rsp + 0x20], edx   ; arg2
180002222  4c 8d 44 24 20              lea     r8, [rsp + 0x20]
180002227  89 44 24 28                 mov     dword ptr [rsp + 0x28], eax   ; arg1
18000222B  48 8d 54 24 28              lea     rdx, [rsp + 0x28]
180002230  48 8b 01                    mov     rax, qword ptr [rcx]
180002233  ff 50 10                    call    qword ptr [rax + 0x10]
180002236  48 83 c4 38                 add     rsp, 0x38
18000223A  c3                          ret
```

### 1.5 vibration — RVA `0x2240` (abs `0x180002240`), 0x2240–0x227A, len 0x3B

```asm
180002240  48 83 ec 38                 sub     rsp, 0x38
180002244  8b c1                       mov     eax, ecx                      ; eax = arg1
180002246  48 8b 0d 43 f6 23 00        mov     rcx, [rip + 0x23f643]         ; rcx = g
18000224D  48 85 c9                    test    rcx, rcx
180002250  74 24                       je      0x180002276
180002252  48 8b 89 c0 01 00 00        mov     rcx, qword ptr [rcx + 0x1c0]  ; g->slot188._Ptr
180002259  48 85 c9                    test    rcx, rcx
18000225C  74 18                       je      0x180002276
18000225E  89 54 24 20                 mov     dword ptr [rsp + 0x20], edx   ; arg2
180002262  4c 8d 44 24 20              lea     r8, [rsp + 0x20]
180002267  89 44 24 28                 mov     dword ptr [rsp + 0x28], eax   ; arg1
18000226B  48 8d 54 24 28              lea     rdx, [rsp + 0x28]
180002270  48 8b 01                    mov     rax, qword ptr [rcx]
180002273  ff 50 10                    call    qword ptr [rax + 0x10]
180002276  48 83 c4 38                 add     rsp, 0x38
18000227A  c3                          ret
```

Int3 padding separates all five: `0x2155–0x215F`, `0x219A–0x219F`, `0x21FB–0x21FF`, `0x223B–0x223F`,
`0x227B–0x227F`.

### 1.6 The ABI, traced register by register

**At the trampoline's own entry** (this is what a detour sees):

| channel | RVA | signature at entry | arg 1 | arg 2 |
|---|---|---|---|---|
| servo-on  | 0x2160 | `void(void)` | — | — |
| servo-off | 0x21A0 | `void(void)` | — | — |
| torque    | 0x21C0 | `void(__int32, __int32)` | `ecx` = direction | `edx` = magnitude |
| viscosity | 0x2200 | `void(__int32, __int32)` | `ecx` = arg1 | `edx` = arg2 |
| vibration | 0x2240 | `void(__int32, __int32)` | `ecx` = arg1 | `edx` = arg2 |

There is **no `this` pointer** in `rcx` — these are free functions. The identity
`mov eax, ecx` (arg 1) + `mov [rsp+0x20], edx` (arg 2) proves the two argument registers, and the
order proves that the *first* game argument is `ecx`. Argument widths are 32-bit (`mov dword ptr`),
so both parameters are `int`-sized, passed **by value** at this boundary.

**At the moment `_Do_call` is entered** (inside the trampoline):

| register | holds |
|---|---|
| `rcx` | the closure object (`g->slotNNN._Ptr`) — MSVC passes the type-erased callable as the implicit `this` |
| `rdx` | **pointer** to a 4-byte stack copy of argument 1 (`&[rsp+0x28]`) |
| `r8`  | **pointer** to a 4-byte stack copy of argument 2 (`&[rsp+0x20]`) |

So the transition is: **by value in `ecx`/`edx` at the trampoline → by pointer (`int&&`) in
`rdx`/`r8` at the `std::function` closure**. This is the MSVC `std::function<R(Args...)>::operator()`
expansion, which forwards `Args&&...` to `_Do_call`. Parameter *order* is preserved: game arg 1 ends
up in the lower stack slot and in `rdx`, game arg 2 in `r8`. Direction is **not** shifted or swapped.

The closure's `_Do_call` then dereferences those pointers and tail-calls the user's callable with the
values by value; that final register assignment depends on the bound callable (free function →
args in `rcx`/`rdx`; a lambda/member → `this` in `rcx`, args in `rdx`/`r8`). That final hop is
downstream of anything a trampoline hook can observe, and it is irrelevant to the detour ABI.

**MinHook detour declaration to use:**

```c
// x64 has one calling convention; these are__fastcall(void) / __fastcall(int,int) in effect.
typedef void (*t_void)(void);
typedef void (*t_int2)(int, int);            // (direction_or_arg1, magnitude_or_arg2)
```

If the detour wants to reproduce the original behaviour it must copy the trampoline's own call
convention exactly: it receives `ecx`/`edx` (by value) and, to forward, must build the two stack
slots and call `[closure_vtbl+0x10]` with `rcx` = closure, `rdx` = `&arg1`, `r8` = `&arg2`. The
simplest safe implementation is to call the original trampoline through the MinHook trampoline
pointer with the same two `int` arguments — no manual `_Do_call` reconstruction needed.

---

## 2. Q2 — can MinHook detour the trampoline itself?

### 2.1 First-5-byte relocation requirement (per function)

MinHook overwrites the entry with a 5-byte `jmp rel32` and relocates whole instructions until at
least 5 bytes are covered. Exact instruction boundaries at each entry:

| RVA | function | bytes stolen | instructions relocated |
|---|---|---|---|
| 0x2160 | servo-on | **11** | `sub rsp,0x28` (4) + `mov rax,[rip+0x23f725]` (7) |
| 0x21A0 | servo-off | **7** | `mov rax,[rip+0x23f6e9]` (7) |
| 0x21C0 | torque | **6** | `sub rsp,0x38` (4) + `mov eax,ecx` (2) |
| 0x2200 | viscosity | **6** | `sub rsp,0x38` (4) + `mov eax,ecx` (2) |
| 0x2240 | vibration | **6** | `sub rsp,0x38` (4) + `mov eax,ecx` (2) |
| 0x2020 | `UnBindAllFB` | **6** | `push rdi` (2) + `sub rsp,0x30` (4) |

No relative branch lies in the stolen bytes of any of the six (the only RIP-relative operand is a
`mov` load, which MinHook rewrites to an absolute form correctly). `UnBindAllFB`'s stolen bytes
contain no branch either — the `push rdi` is unwound by the `.pdata` record pointing at `0x2020`.
There is no short-jump / hot-patch conflict: `0x21A0`'s 7-byte `mov` is the narrowest case and it is
a `mov`, not a branch.

### 2.2 Reachability: are they address-taken, and how many callers?

* An exhaustive scan of the whole image for the little-endian 8-byte absolute value of each of
  `0x2020, 0x2160, 0x21A0, 0x21C0, 0x2200, 0x2240` returns **0 occurrences for every one of them**.
  They are not in any vtable, not in any static table, and never indirect-called.
* A byte scan for `E8`/`E9 rel32` covering the entire `.text` finds exactly these references:

| target | site RVA | kind | containing function |
|---|---|---|---|
| 0x2160 servo-on  | 0x2B7B8 | `call` | 0x148A0–0x2D274 |
| 0x21A0 servo-off | 0xC053  | `call` | 0x6E70–0xCC5C |
| 0x21C0 torque    | 0x2BCDB | `call` | 0x148A0–0x2D274 |
| 0x2200 viscosity | 0x2B7E3, 0x2BFC1 | `call` | 0x148A0–0x2D274 |
| 0x2240 vibration | 0x2B7C1, 0x2B7EC, 0x2BE91 | `call` | 0x148A0–0x2D274 |
| 0x2020 `UnBindAllFB` | 0x5FA60 | `jmp` (the exported thunk) | — |

No `jmp`-based entries to the five invoke trampolines exist besides those calls.

### 2.3 Recommendation

**Detour the trampoline RVAs, not the call sites.**

| channel | recommended detour RVA | abs | why |
|---|---|---|---|
| servo-on  | **0x2160** | 0x180002160 | single call site 0x2B7B8, but the trampoline is the stable, semantically-meaningful entry |
| servo-off | **0x21A0** | 0x1800021A0 | single call site 0xC053 |
| torque    | **0x21C0** | 0x1800021C0 | call site 0x2BCDB only; args already set up in `ecx`/`edx` at entry |
| viscosity | **0x2200** | 0x180002200 | 2 call sites — one hook covers both |
| vibration | **0x2240** | 0x180002240 | 3 call sites — one hook covers all three |
| (optional) `UnBindAllFB` | **0x2020** | 0x180002020 | exported as a 5-byte `jmp` thunk at 0x5FA60; hook the body, not the thunk |

Justification:

1. Each trampoline is 0x20–0x3B bytes long with `int3` padding after it, so a 5-byte patch never
   bleeds into the next function, and the relocated bytes (6–11) still fit inside the same body.
2. There are zero relative branches in the patched bytes and zero address-taken references, so there
   is no path that bypasses the hook and no relocation hazard.
3. Hooking the trampoline catches *every* caller with one hook — required for viscosity (2 sites)
   and vibration (3 sites), and future-proof if a patch adds callers.
4. The call sites live inside a 0x18,9xx-byte mega-function (`0x148A0–0x2D274`) that has SEH and a
   large unwind record; patching its interior is legal but strictly more invasive, and the two
   viscosity / three vibration sites would each need their own hook.
5. The argument registers are already final at the trampoline entry (`ecx`/`edx`), so a detour sees
   exactly the values the game computed — no need to re-derive them.

Caveat to keep in mind: because servo-on is gated on the servo-off slot (`cmp [rax+0x80],0`), a
detour of `0x2160` only fires when servo-off is bound and only invokes slot `+0x08`. If a hook needs
to run even when the servo-off slot is empty, hook `0x2160` anyway but note it will not be reached
in that state — the game never calls it then either.

---

## 3. Q3 — singleton access path (summary)

* `0x180241890` is the address of a **`cUE4Interface*` pointer variable** in `.data`
  (8 bytes at absolute `0x180241890`). It is written in exactly one place: RVA `0x5E0C3`
  (`mov qword ptr [rip + 0x1e37c6], rax`), inside the lazy-create routine, guarded by the
  double-check at RVA `0x5E0BA`.
* The object it points to is `operator new(0x1C8, 8)`, allocated at RVA `0x5E072`; it has a vtable
  pointer at `+0x00`, seven `std::function` slots at `+0x08..+0x188`, total size 0x1C8. On allocation
  failure the global is set to `0`.
* Therefore at runtime:

```c
uint8_t* base = (uint8_t*)GetModuleHandleW(L"IndRun.dll");   // or the injected HMODULE
void* g = *reinterpret_cast<void**>(base + 0x241890);        // cUE4Interface*
if (!g) return;                                              // legitimately null before first use
void* closure = *reinterpret_cast<void**>((uint8_t*)g + SLOT + 0x38);  // std::function::_Ptr
if (!closure) return;                                        // slot not bound by the game
auto do_call = *reinterpret_cast<void*(**)(void*, void*, void*)>(closure);  // [vtbl+0x10]
```

with `SLOT` ∈ {0x08 servo-on, 0x48 servo-off, 0x88 centering(void), 0xC8 centering(int),
0x108 torque, 0x148 viscosity, 0x188 vibration}.

Never cache `g` or `closure` across frames: both are created lazily and `closure` is nulled by
`UnBindAllFB`.

---

## 4. Q4 — every call site of the invoke trampolines

### 4.1 torque `0x21C0` — the only call site, RVA 0x2BCDB

```asm
18002BCAB  3b 9f 3c 16 00 00           cmp     ebx, dword ptr [rdi + 0x163c]   ; same as last torque?
18002BCB1  74 2d                       je      0x18002BCE0                    ; -> skip
18002BCB3  38 87 4d 16 00 00           cmp     byte ptr [rdi + 0x164d], al     ; gate flag (al == 0)
18002BCB9  75 25                       jne     0x18002BCE0
18002BCBB  45 32 f6                    xor     r14b, r14b
18002BCBE  38 87 12 58 00 00           cmp     byte ptr [rdi + 0x5812], al     ; FFB enable gate
18002BCC4  74 1a                       je      0x18002BCE0                    ; -> skip when disabled
18002BCC6  8b cb                       mov     ecx, ebx                       ; arg = torque value
18002BCC8  e8 23 d4 00 00              call    0x1800390F0                    ; map torque -> magnitude
18002BCCD  8b d0                       mov     edx, eax                       ; edx = MAGNITUDE (arg 2)
18002BCCF  85 db                       test    ebx, ebx                       ; torque sign
18002BCD1  b9 ff ff ff ff              mov     ecx, 0xFFFFFFFF                ; DIRECTION = -1  <== constant
18002BCD6  7f 03                       jg      0x18002BCDB                    ; torque > 0 -> keep -1
18002BCD8  41 8b cd                    mov     ecx, r13d                     ; DIRECTION = r13d <== runtime
18002BCDB  e8 e0 64 fd ff              call    0x1800021C0                    ; torque trampoline
18002BCE0  89 9f 3c 16 00 00           mov     dword ptr [rdi + 0x163c], ebx  ; remember last torque
```

`ebx` (the torque) itself comes from a long chain in the same block: `cvttsd2si ebx,xmm0` at
`0x2BBFD`; a `clamp(-127, +127)` via `mov r9d,0xFFFFFF81` / `mov r8d,0x7F` at `0x2BC46`/`0x2BC4C`
calling `0x180004B80`; `and eax,0xF000` at `0x2BC80` with a conditional negate at
`0x2BC85–0x2BC8E`; a zeroing `cmovne` at `0x2BCA8` driven by `call 0x180002630` at `0x2BC9C`.
Direction is therefore never derived from the torque value except through the `jg` at `0x2BCD6`.

### 4.2 vibration `0x2240` — three call sites

```asm
; (a) RVA 0x2B7C1 — zero/zero, "stop vibration"
18002B7B8  e8 a3 69 fd ff              call    0x180002160                    ; servo-on
18002B7BD  33 d2                       xor     edx, edx                       ; MAGNITUDE = 0
18002B7BF  33 c9                       xor     ecx, ecx                       ; DIRECTION = 0
18002B7C1  e8 7a 6a fd ff              call    0x180002240                    ; vibration

; (b) RVA 0x2B7EC — zero/zero
18002B7DF  33 d2                       xor     edx, edx                       ; MAGNITUDE = 0
18002B7E1  33 c9                       xor     ecx, ecx                       ; DIRECTION = 0
18002B7E3  e8 18 6a fd ff              call    0x180002200                    ; viscosity
18002B7E8  33 d2                       xor     edx, edx                       ; MAGNITUDE = 0
18002B7EA  33 c9                       xor     ecx, ecx                       ; DIRECTION = 0
18002B7EC  e8 4f 6a fd ff              call    0x180002240                    ; vibration

; (c) RVA 0x2BE91 — computed value + computed magnitude
18002BE66  0f 28 ce                    movaps  xmm1, xmm6
18002BE69  48 8b ce                    mov     rcx, rsi
18002BE6C  e8 4f 8d fd ff              call    0x180004BC0                    ; f(xmm1,xmm3) -> xmm0
18002BE71  f3 0f 2c d8                 cvttss2si ebx, xmm0                    ; ebx = (int)xmm0
18002BE75  39 9f 40 16 00 00           cmp     dword ptr [rdi + 0x1640], ebx
18002BE7B  74 1c                       je      0x18002BE99                    ; unchanged -> skip
18002BE7D  f3 41 0f 59 fc              mulss   xmm7, xmm12
18002BE82  80 bf 12 58 00 00 00        cmp     byte ptr [rdi + 0x5812], 0     ; FFB enable gate
18002BE89  74 0b                       je      0x18002BE96
18002BE8B  f3 0f 2c d7                 cvttss2si edx, xmm7                    ; edx = MAGNITUDE
18002BE8F  8b cb                       mov     ecx, ebx                       ; ecx = DIRECTION = (int)xmm0
18002BE91  e8 aa 63 fd ff              call    0x180002240                    ; vibration
18002BE96  45 32 f6                    xor     r14b, r14b
18002BE99  89 9f 40 16 00 00           mov     dword ptr [rdi + 0x1640], ebx
```

### 4.3 viscosity `0x2200` — two call sites

```asm
; (a) RVA 0x2B7E3 — zero/zero, gated by flag [rdi+0x4EED] and a float compare
18002B7C6  80 bf ed 4e 00 00 00        cmp     byte ptr [rdi + 0x4eed], 0
18002B7CD  74 22                       je      0x18002B7F1
18002B7CF  48 8b 87 58 3b 00 00        mov     rax, qword ptr [rdi + 0x3b58]
18002B7D6  0f 2f b0 4c 09 00 00        comiss  xmm6, dword ptr [rax + 0x94c]
18002B7DD  76 12                       jbe     0x18002B7F1
18002B7DF  33 d2                       xor     edx, edx                       ; arg2 = 0
18002B7E1  33 c9                       xor     ecx, ecx                       ; arg1 = 0
18002B7E3  e8 18 6a fd ff              call    0x180002200                    ; viscosity

; (b) RVA 0x2BFC1 — arg1 = r13d, arg2 = (int)float
18002BFA2  f3 0f 2c d8                 cvttss2si ebx, xmm0
18002BFA6  39 9f 44 16 00 00           cmp     dword ptr [rdi + 0x1644], ebx
18002BFAC  75 05                       jne     0x18002BFB3
18002BFAE  45 84 f6                    test    r14b, r14b
18002BFB1  74 13                       je      0x18002BFC6
18002BFB3  80 bf 12 58 00 00 00        cmp     byte ptr [rdi + 0x5812], 0
18002BFBA  74 0a                       je      0x18002BFC6
18002BFBC  8b d3                       mov     edx, ebx                       ; arg2 = MAGNITUDE
18002BFBE  41 8b cd                    mov     ecx, r13d                     ; arg1 = r13d
18002BFC1  e8 3a 62 fd ff              call    0x180002200                    ; viscosity
18002BFC6  89 9f 44 16 00 00           mov     dword ptr [rdi + 0x1644], ebx
```

### 4.4 servo-on `0x2160` — one call site, RVA 0x2B7B8; servo-off `0x21A0` — one, RVA 0xC053

```asm
18002B79D  80 bf 12 58 00 00 00        cmp     byte ptr [rdi + 0x5812], 0     ; FFB enable gate
18002B7A4  74 4b                       je      0x18002B7F1
18002B7A6  83 bf e8 04 00 00 0a        cmp     dword ptr [rdi + 0x4e8], 0xa   ; state == 10 ?
18002B7AD  75 17                       jne     0x18002B7C6
18002B7AF  80 bf 4d 16 00 00 00        cmp     byte ptr [rdi + 0x164d], 0
18002B7B6  75 0e                       jne     0x18002B7C6
18002B7B8  e8 a3 69 fd ff              call    0x180002160                    ; servo-on (no args)

18000C049  41 80 be 12 58 00 00 00     cmp     byte ptr [r14 + 0x5812], 0     ; FFB enable gate
18000C051  74 05                       je      0x18000C058
18000C053  e8 48 61 ff ff              call    0x1800021A0                    ; servo-off (no args)
```

Neither consumes an argument register. The single gate that all five channels share is the byte at
`device + 0x5812` (FFB master enable); the torque/vibration/viscosity calls additionally check the
"value changed since last tick" fields at `+0x163C` (torque), `+0x1640` (vibration), `+0x1644`
(viscosity), and the torque/others also consult `+0x164D`, `+0x4E8`, `+0x4EED`.

### 4.5 Complete set of distinct values written to the direction register

| value | instruction | RVA | abs | channel | meaning |
|---|---|---|---|---|---|
| `0xFFFFFFFF` (`-1`) | `b9 ff ff ff ff  mov ecx, 0xFFFFFFFF` | **0x2BCD1** | 0x18002BCD1 | torque | taken when the computed torque `> 0` (`jg` at 0x2BCD6). The only literal direction in the binary; out-of-enum sentinel = "positive/derived direction" |
| `r13d` | `41 8b cd  mov ecx, r13d` | **0x2BCD8** | 0x18002BCD8 | torque | taken when torque `<= 0`; value = `(uint32_t)(uint8_t)device[0x1678]` (see §5) |
| `r13d` | `41 8b cd  mov ecx, r13d` | **0x2BFBE** | 0x18002BFBE | viscosity | unconditional arg1 of the computed-viscosity path |
| `0x00000000` | `33 c9  xor ecx, ecx` | **0x2B7BF** | 0x18002B7BF | vibration | neutral / stop |
| `0x00000000` | `33 c9  xor ecx, ecx` | **0x2B7E1** | 0x18002B7E1 | viscosity | neutral / stop |
| `0x00000000` | `33 c9  xor ecx, ecx` | **0x2B7EA** | 0x18002B7EA | vibration | neutral / stop |
| `ebx` = `(int)xmm0` | `8b cb  mov ecx, ebx` | **0x2BE8F** | 0x18002BE8F | vibration | computed vibration value/level from `call 0x180004BC0` at 0x2BE6C |

No other write to `ecx` feeding a torque/viscosity/vibration call exists anywhere in the image; the
five invoke trampolines have no other callers (§2.2).

---

## 5. Q5 — where `r13d` comes from (`eTrqueDirection` values)

* `eTrqueDirection` is `nsFeedbackDefine::eTrqueDirection`; the mangled binder name
  `...$$A6AXW4eTrqueDirection@nsFeedbackDefine@@H@Z...` proves the enum is 4 bytes wide (`W4`) and is
  passed as the first parameter, followed by `int`.
* `r13d` is a **callee-saved register used as a long-lived state variable** inside the mega-function
  `0x148A0–0x2D274`. Every write to it in that function:

| RVA | instruction | note |
|---|---|---|
| 0x1B531 | `41 bd 01 00 00 00  mov r13d, 1` | value `1`, in an unrelated region of the function |
| 0x1E5D0 | `mov r13d, 1` | value `1` |
| 0x1E998 | `mov r13d, 1` | value `1` |
| 0x1E366 / 0x1E51D / 0x1E523 | `mov r13, rax` … | 64-bit pointer arithmetic (not the direction) |
| 0x1E8AF | `4c 8b 2d ea 2f 22 00  mov r13, [rip+0x222fea]` | pointer load |
| **0x2C965** | `44 0f b6 af 78 16 00 00  movzx r13d, byte ptr [rdi+0x1678]` | **value = zero-extended byte at `device+0x1678`, domain 0x00–0xFF** |
| 0x2D1C0 | `41 5d  pop r13` | epilogue restore |

* The only definition of `r13d` that lies in the same region as the torque/viscosity call sites and
  has a value domain compatible with a direction/enum is `0x2C965`. Its value is produced by the
  table-search loop immediately before it:

```asm
18002C930  ...        movsxd  rax, r13d
18002C934  ...        add     rax, rcx
18002C937  ...        movss   xmm1, dword ptr [r15 + rax*4 + 0x9e764]   ; float table
           ...        cvtps2pd / mulsd / cvtss2sd / comisd
18002C95?  ...        ja      0x2C95C
           ...        inc     r13d
           ...        cmp     r13d, r8d
           ...        jle     0x2C930
           ...        jmp     0x2C96D
18002C95C  44 89 af 78 16 00 00     mov     dword ptr [rdi + 0x1678], r13d   ; store index/level
18002C965  44 0f b6 af 78 16 00 00  movzx   r13d, byte ptr [rdi + 0x1678]    ; reload as byte
18002C96D  48 8b cf                 mov     rcx, rdi
           ...                       call    0x180003D40
           ...                       movzx   edx, r13b
           ...                       call    0x180005760                    ; consumer of r13b
```

  `device+0x1678` has exactly **two** references in the entire decoded image: the dword store at
  `0x2C95C` and the byte load at `0x2C965`. The value written there is an index/level found by a
  float-table search, and the same byte is consumed at `0x2C96D` via `movzx edx, r13b` — so the
  quantity is a **gear/level-style index (0..255)**, not a 2-state direction.

* Only one predecessor reaches the `movzx` at `0x2C965`: `je` at RVA `0x2C406` → `0x2C965`. The two
  locations are in the same function (`0x148A0–0x2D274`), and `r13` is non-volatile, so the value
  loaded at `0x2C965` survives across the intervening calls and is what the torque/viscosity site at
  `0x2BCDB` / `0x2BFC1` observes (the FFI block at `0x2B600–0x2C200` contains **no** writer of
  `r13d` at all — a scan of every `r13`-writing instruction in `0x2B600–0x2C200` returns nothing).

**Conclusion for the detour:** do not assume `-1` is the only direction. Observed runtime domain is
`{ -1 } ∪ { 0 } ∪ { 1 } ∪ [0,255]`, i.e. treat the parameter as a plain `int` and log/pass it
through unchanged. `-1` (`0xFFFFFFFF`) is the only literal constant and means "computed torque was
positive"; `0` means "neutral/stop"; the `r13d` path carries the current gear/level index
(`(uint8_t)device[0x1678]`), and `1` appears as a possible state value from the `mov r13d,1` sites
in other regions of the same function.

---

## 6. Q6 — `UnBindAllFB` at RVA 0x2020 (abs 0x180002020)

Exported as `?UnBindAllFB@cUE4Interface@@QEAAXXZ` → the export thunk at RVA `0x5FA60` is
`e9 bb 25 fa ff` = `jmp 0x180002020`, so the export and the body are the same function.

`.pdata`: (0x2020, 0x2155); `UNWIND_INFO` at RVA `0x231120` = `11 14 04 00 14 34 08 00 06 52 02 70`
(version 1, flags `UNW_FLAG_UHANDLER` = 2, prologsize 0x14, four codes: `UWOP_SAVE_NONVOL rbx`
@offset 0x14, `UWOP_PUSH_NONVOL rdi` @offset 2, `UWOP_ALLOC_SMALL 0x30` @offset 6, plus the SEH
cookie store). Prologue bytes:

```asm
180002020  40 57                       push    rdi
180002022  48 83 ec 30                 sub     rsp, 0x30
180002026  48 c7 44 24 20 fe ff ff ff  mov     qword ptr [rsp + 0x20], 0xFFFFFFFFFFFFFFFE   ; SEH cookie
18000202F  48 89 5c 24 40              mov     qword ptr [rsp + 0x40], rbx
180002034  48 8b 05 55 f8 23 00        mov     rax, [rip + 0x23f855]        ; rax = g
18000203B  48 85 c0                    test    rax, rax
18000203E  0f 84 06 01 00 00           je      0x18000214A                 ; !g -> epilogue
```

It then executes **seven unrolled iterations**, one per slot, in ascending order:

```asm
180002044  48 8d 58 08                 lea     rbx, [rax + 8]               ; slot +0x08
180002048  48 8b 4b 38                 mov     rcx, qword ptr [rbx + 0x38]  ; _Ptr
18000204C  33 ff                       xor     edi, edi                     ; edi = 0 (null), once
18000204E  48 85 c9                    test    rcx, rcx
180002051  74 17                       je      0x18000206A                  ; empty -> next slot
180002053  48 8b 01                    mov     rax, qword ptr [rcx]         ; closure vtable
180002056  48 3b cb                    cmp     rcx, rbx
180002059  0f 95 c2                    setne   dl                           ; dl = (not stored in-place) -> delete
18000205C  ff 50 20                    call    qword ptr [rax + 0x20]       ; _Delete_this(bool)
18000205F  48 89 7b 38                 mov     qword ptr [rbx + 0x38], rdi  ; _Ptr = 0
180002063  48 8b 05 26 f8 23 00        mov     rax, [rip + 0x23f826]        ; reload g
```

| iteration | `lea` site | slot offset | `_Ptr` read | teardown call |
|---|---|---|---|---|
| 1 | `0x180002044` `lea rbx,[rax+8]` | `+0x08` (servo-on) | `[rbx+0x38]` @0x2048 | 0x205C |
| 2 | `0x18000206A` `lea rbx,[rax+0x48]` | `+0x48` (servo-off) | @0x206E | 0x2080 |
| 3 | `0x18000208E` `lea rbx,[rax+0x88]` | `+0x88` (centering void) | @0x2095 | 0x20A7 |
| 4 | `0x1800020B5` `lea rbx,[rax+0xc8]` | `+0xC8` (centering int) | @0x20BC | 0x20CE |
| 5 | `0x1800020DC` `lea rbx,[rax+0x108]` | `+0x108` (torque) | @0x20E3 | 0x20F5 |
| 6 | `0x180002103` `lea rbx,[rax+0x148]` | `+0x148` (viscosity) | @0x210A | 0x211C |
| 7 | `0x18000212A` `lea rbx,[rax+0x188]` | `+0x188` (vibration) | @0x2131 | 0x2143 |

Epilogue:

```asm
18000214A  48 8b 5c 24 40              mov     rbx, qword ptr [rsp + 0x40]
18000214F  48 83 c4 30                 add     rsp, 0x30
180002153  5f                          pop     rdi
180002154  c3                          ret
```

**This confirms the authoritative slot list**: exactly seven slots at `+0x08, +0x48, +0x88, +0xC8,
+0x108, +0x148, +0x188`, each with its `_Ptr` at `slot+0x38`, each destroyed through closure vtable
slot 4 (`[vtbl+0x20]`) with `dl` = "was heap-allocated", then zeroed. Note that the singleton pointer
is **reloaded from `[0x241890]` after each slot** (5 reloads at 0x2063, 0x2087, 0x20AE, 0x20D5,
0x20FC, 0x2123) — a hook that nulls the global mid-walk is therefore tolerated by the code.

---

## 7. Hook skeleton (derived from the above)

```c
#include <windows.h>
#include <stdint.h>

static uint8_t* g_base;                                   // HMODULE of IndRun.dll
#define RVA_SERVO_ON   0x2160
#define RVA_SERVO_OFF  0x21A0
#define RVA_TORQUE     0x21C0
#define RVA_VISCOSITY  0x2200
#define RVA_VIBRATION  0x2240
#define RVA_UNBINDALL  0x2020

typedef void (*fn_void)(void);
typedef void (*fn_int2)(int, int);

static fn_void o_servo_on, o_servo_off, o_unbindall;
static fn_int2 o_torque, o_viscosity, o_vibration;

static void* g_singleton(void) {                          // *0x241890, single deref
    return *(void**)(g_base + 0x241890);
}
static void* slot_ptr(int slot_off) {                     // std::function::_Ptr
    void* g = g_singleton();
    return g ? *(void**)((uint8_t*)g + slot_off + 0x38) : NULL;
}

static void __fastcall hk_servo_on(void)  { /* ... */ o_servo_on(); }
static void __fastcall hk_servo_off(void) { /* ... */ o_servo_off(); }
static void __fastcall hk_torque(int direction, int magnitude) {
    /* direction: -1 literal path, 0 neutral, r13d = (uint8_t)dev[0x1678] */
    o_torque(direction, magnitude);
}
static void __fastcall hk_viscosity(int a1, int a2) { o_viscosity(a1, a2); }
static void __fastcall hk_vibration(int a1, int a2) { o_vibration(a1, a2); }

void install(void) {
    MH_Initialize();
    MH_CreateHook(g_base + RVA_SERVO_ON,  hk_servo_on,  (void**)&o_servo_on);
    MH_CreateHook(g_base + RVA_SERVO_OFF, hk_servo_off, (void**)&o_servo_off);
    MH_CreateHook(g_base + RVA_TORQUE,    hk_torque,    (void**)&o_torque);
    MH_CreateHook(g_base + RVA_VISCOSITY, hk_viscosity, (void**)&o_viscosity);
    MH_CreateHook(g_base + RVA_VIBRATION, hk_vibration, (void**)&o_vibration);
    MH_EnableHook(MH_ALL_HOOKS);
}
```

`o_*` are the MinHook trampolines; on x64 the calling convention is the single Microsoft ABI, so a
plain `void(int,int)` detour matches `__fastcall` exactly (no `this`, no hidden parameter). Both
`int` arguments arrive **by value** in `ecx`/`edx`.

---

## 8. Artifact index

| file | content |
|---|---|
| `D:\INDAC\DGLab_IDAC\re\eng2.py` | PE/capstone engine (ImageBase, sections, `.pdata` function list, `insns()`, `scan_calls()` byte-pattern call/jmp scan) |
| `D:\INDAC\DGLab_IDAC\re\final2.out` | byte-annotated listings of all six functions + aligned call-site windows + immediate tables |
| `D:\INDAC\DGLab_IDAC\re\final3.out` | `Bind*`/`UnBind*` export names and bodies, `r13d` origin, IAT resolution, address-taken scan, unwind summary, first-5-byte boundaries |
| `D:\INDAC\DGLab_IDAC\re\final4.out` | predecessor scan of the FFI block, refs to `device+0x1678`, gate-byte reference counts |
| `D:\INDAC\DGLab_IDAC\re\flow13.py` | forward dataflow over the `0x148A0–0x2D274` CFG used to pin `r13d` |
| `D:\INDAC\DGLab_IDAC\re\dump_all.py` / `dump_all.txt` | regeneration of every listing in sections 1, 2 and 6 of this report |
