# VBoxVMI protocol, version 2

VBoxVMI is the virtual machine introspection protocol served by a VirtualBox VM
process built with `VBOX_WITH_VMI` (see `VBoxVMI-design.md`). The C definitions
are in `include/VBox/vmm/vmi.h`; a Rust client is `vboxvmi`
(https://github.com/Wenzel/rs-vboxvmi).

## Transport

- A Unix domain socket of type `SOCK_SEQPACKET`, one per VM. Each `send` is
  one message and each `recv` returns exactly one message, so there is no
  framing beyond the header below.
- Path: VM config value `VMI/SocketPath`, extradata
  `VBoxInternal/VMI/SocketPath`. Default `<VirtualBox user home>/vmi/<VM UUID>.sock`
  (the user home is `VBOX_USER_HOME` or the per-user VirtualBox config directory).
- The socket is mode 0600 and the server only serves peers running under the
  VM process' effective uid (`SO_PEERCRED`).
- The socket's directory must be owned by that uid and not writable by group
  or others (unless sticky), and the path must be absolute.
- A client must read its replies: a reply the server cannot send within 5 s
  gets the client disconnected.
- One client at a time. The server greets a refused connection with an error
  status (see Handshake) and closes it.

## Messages

All integers are little endian. A message is a 16 byte header followed by a
type specific body. Maximum message size: 64 KiB, header included.

| Offset | Size | Field    | Meaning |
|-------:|-----:|----------|---------|
| 0      | 4    | `cbMsg`  | Size of the whole message, header included. Must equal the received size. |
| 4      | 4    | `idReq`  | Request ID chosen by the client, echoed in the reply. 0 in greeting and asynchronous events. |
| 8      | 2    | `uType`  | Message type (command or event code). A reply has the type of its request. |
| 10     | 2    | `fFlags` | Bit 0 `VMI_MSG_F_REPLY`: the message is a reply. Other bits are 0. |
| 12     | 4    | `rc`     | VirtualBox status code (`iprt/err.h`, `VBox/err.h`): the result in replies and in the greeting, 0 in requests. |

Requests have `fFlags` and `rc` zero. A request whose header is malformed
(size mismatch, nonzero `fFlags` or `rc`, larger than 64 KiB) is a protocol
error: the server drops the connection without replying.

Client IDs are not interpreted by the server; use a fresh nonzero ID per
request. Events with `idReq` 0 can arrive between replies on the same ordered
connection. An event has `fFlags` and `rc` zero.

### Message types

| Code     | Name            | Body | Since |
|----------|-----------------|------|-------|
| `0x0001` | `VMI_MSG_HELLO` | `VMIHELLO` | 1 |
| `0x0002` | `VMI_MSG_PHYS_READ` | see Physical memory read | 1 |
| `0x0003` | `VMI_MSG_RAM_RANGES` | see RAM ranges | 1 |
| `0x0004` | `VMI_MSG_PAUSE` | see Pause and resume | 1 |
| `0x0005` | `VMI_MSG_RESUME` | see Pause and resume | 1 |
| `0x0006` | `VMI_MSG_VCPU_COUNT` | see vCPU count | 1 |
| `0x0007` | `VMI_MSG_REGS_READ` | see Registers | 1 |
| `0x0008` | `VMI_MSG_REGS_WRITE` | see Registers | 1 |
| `0x0009` | `VMI_MSG_PHYS_WRITE` | see Physical memory write | 1 |
| `0x000a` | `VMI_MSG_INTERCEPT_SET` | `VMIINTERCEPT` | 2 |
| `0x000b` | `VMI_MSG_INTERCEPT_CLEAR` | `VMIINTERCEPT` | 2 |
| `0x000c` | `VMI_MSG_EVENT_REPLY` | `VMIEVENTREPLY` | 2 |
| `0x000d` | `VMI_MSG_EVENT` | `VMIEVENT` (server to client only) | 2 |
| `0x000e` | `VMI_MSG_PAGE_ACCESS_SET` | `VMIPAGEACCESS` | 2 |
| `0x000f` | `VMI_MSG_PAGE_ACCESS_GET` | `VMIPAGEACCESS` | 2 |

Any other type is answered with `VERR_NOT_SUPPORTED` (-37) and an empty body;
the connection stays up.

## Handshake

`VMIHELLO` body (8 bytes):

| Offset | Size | Field        | Value |
|-------:|-----:|--------------|-------|
| 0      | 4    | `u32Magic`   | `0x494d5642`, the bytes `"BVMI"` |
| 4      | 4    | `u32Version` | Protocol version of the sender, `2` |

1. **Greeting.** Right after accepting, the server sends `VMI_MSG_HELLO` with
   `idReq` 0, `fFlags` 0, its own magic and version, and `rc`:
   - `VINF_SUCCESS` (0): the client is served;
   - `VERR_RESOURCE_BUSY` (-138): another client is connected; the server closes;
   - `VERR_ACCESS_DENIED` (-38): the peer runs as another user; the server closes.
2. **Hello request.** The client sends `VMI_MSG_HELLO` with its magic and version.
3. **Hello reply.** The server replies (`VMI_MSG_F_REPLY`, same `idReq`) with its
   magic and version, and `rc`:
   - `VINF_SUCCESS`: handshake done, other requests may follow;
   - `VERR_INVALID_MAGIC` (-3) or `VERR_VERSION_MISMATCH` (-11): the server closes.
     Versions must match exactly.

Any other request before a successful handshake is answered with
`VERR_WRONG_ORDER` (-22) and the server closes. A second hello after the
handshake, whatever its body, is answered with `VERR_WRONG_ORDER` and the connection stays up.

## Physical memory read

`VMI_MSG_PHYS_READ` reads guest physical memory: several ranges per request,
with a status per guest page (4 KiB).

Request body: one or more `VMIPHYSRANGE` (16 bytes each):

| Offset | Size | Field         | Meaning |
|-------:|-----:|---------------|---------|
| 0      | 8    | `GCPhys`      | Guest physical address of the first byte. |
| 8      | 4    | `cb`          | Number of bytes, nonzero. The range must not wrap around. |
| 12     | 4    | `u32Reserved` | 0. |

Reply body, in request order:

1. one `int32_t` status per guest page that a range overlaps (a range of `cb`
   bytes at `GCPhys` overlaps `((GCPhys + cb - 1) >> 12) - (GCPhys >> 12) + 1`
   pages), then
2. the `cb` bytes of every range.

A page's status is `VINF_SUCCESS` (0) if it was read, otherwise the reason it
is unreadable, and its bytes are zeros:

- `VERR_PGM_PHYS_PAGE_RESERVED` (-1618): MMIO, including the special aliased
  MMIO pages such as the local APIC page. Devices are never accessed.
- `VERR_PGM_INVALID_GC_PHYSICAL_ADDRESS` (-1605): no memory at that address.

RAM, ROM and MMIO2 (e.g. VRAM) pages are readable. An unreadable page never
fails the request: the header `rc` is `VINF_SUCCESS` whenever the request is
well formed. Otherwise it is, with an empty body and the connection kept:

- `VERR_INVALID_PARAMETER` (-2): empty body or not a multiple of 16 bytes, a
  range of 0 bytes, one that wraps around, or a nonzero reserved field;
- `VERR_BUFFER_OVERFLOW` (-41): the reply would exceed the maximum message
  size, i.e. `16 + 4 * pages + bytes > 65536`.

So one request reads at most 15 full pages (61440 bytes). A client splits
bigger reads into several requests. The reads are served on the server thread,
without waiting for a vCPU, and the VM keeps running unless it is paused: a
range is not read atomically.

## Physical memory write

`VMI_MSG_PHYS_WRITE` writes guest physical memory in one request. Its body
starts with a little-endian `uint32_t cRanges` (nonzero), followed by exactly
`cRanges` `VMIPHYSRANGE` descriptors (same layout as Physical memory read),
then the `cb` bytes for each range concatenated in descriptor order. The count
distinguishes descriptors from data. The request, including its 16-byte header,
must fit in 64 KiB.

Reply body: one little-endian signed `int32_t` VBox status per 4 KiB guest page
overlapped by each range, in range order, including repeated pages in
overlapping ranges. A page succeeds with `VINF_SUCCESS` (0); otherwise its
bytes are not written:

- `VERR_PGM_PHYS_PAGE_RESERVED` (-1618): any non-RAM page (MMIO,
  special aliased MMIO, MMIO2, ROM, or ROM shadow), or a RAM page with an
  active access handler. No MMIO/device write handlers are called; access
  handlers are not bypassed.
- `VERR_PGM_INVALID_GC_PHYSICAL_ADDRESS` (-1605): unmapped guest physical
  address.
- Another VirtualBox status: the writable page mapping could not be made.

Only RAM pages without active access handlers can be written. Writable
mappings perform VirtualBox's shared/zero-page conversion and write-monitor
handling.
If one page fails, the server continues with the remaining pages and the
reply header `rc` is `VINF_SUCCESS`; this is not an atomic multi-page write.
Ranges and pages may overlap, and later writes win. Like reads, writes are
served on the server thread while the VM may be running; pause it when
consistent guest state is required.

Before writing any byte, the server validates every descriptor and the exact
payload length. An invalid request has an empty reply body, keeps the
connection, and returns:

- `VERR_INVALID_PARAMETER` (-2): missing/zero count, insufficient
  descriptors, zero-sized or wrapping range, nonzero reserved field, or
  trailing/missing payload bytes;
- `VERR_BUFFER_OVERFLOW` (-41): `16 + 4 * pages > 65536` for the reply.

## RAM ranges

`VMI_MSG_RAM_RANGES` returns the guest's RAM ranges, the physical address ranges
backed by RAM (not ROM, MMIO or MMIO2), contiguous ones merged. The end of the
highest is the guest's max physical address, which is above 4 GiB for guests
with RAM above the MMIO hole.

Request body: empty (anything else: `VERR_INVALID_PARAMETER`, connection kept).

Reply body: `VMIRAMRANGE` entries (16 bytes each), sorted by address:

| Offset | Size | Field         | Meaning |
|-------:|-----:|---------------|---------|
| 0      | 8    | `GCPhysFirst` | Guest physical address of the first byte. |
| 8      | 8    | `GCPhysLast`  | Guest physical address of the last byte. |

## Pause and resume

`VMI_MSG_PAUSE` pauses the VM and `VMI_MSG_RESUME` resumes it, with
VirtualBox's standard suspend and resume (those of `VBoxManage controlvm
pause`/`resume`): by the time the reply is sent, `VBoxManage showvminfo` and
the GUI show the VM as paused or running.

Request body: empty. Reply body: empty.

- **Synchronous.** The pause reply is sent once every vCPU has stopped
  executing guest code; the VM stays paused until a resume, from the client or
  anyone else.
- **Idempotent, not counted.** Pausing a paused VM and resuming a running one
  succeed and change nothing. One resume undoes any number of pauses, whoever
  made them (the client, `VBoxManage`, the GUI).

Reply `rc`, with the connection kept:

- `VINF_SUCCESS` (0): the VM is paused (`VMI_MSG_PAUSE`) or running
  (`VMI_MSG_RESUME`);
- `VERR_INVALID_PARAMETER` (-2): nonempty body;
- `VERR_VM_INVALID_VM_STATE` (-1907), the request is refused; it may succeed
  once the VM settles:
  - the VM is neither running nor paused (powering on or off, restoring or
    saving its state, live snapshot, teleporting, ...);
  - VirtualBox suspended the VM briefly for a configuration change (medium or
    network attachment), during which it still shows as running;
  - for `VMI_MSG_RESUME`: the VM was paused by the host's power management
    (suspend), which resumes it itself, or it is a teleport source or target
    that only VirtualBox may resume (also a paused VM whose teleport failed,
    until it is resumed through VirtualBox);
- another VirtualBox status if the suspend or resume itself failed.

The server records which pause is the client's: a `VMI_MSG_PAUSE` that
suspended the VM (not one that found it paused), until the VM is resumed by
anyone, the client's `VMI_MSG_RESUME` included. On disconnect, the server
synchronously resumes such a client-owned pause before ending the session.
A pause by someone else is never resumed: this includes a VM paused before the
client's no-op pause, and an external pause after someone resumed the client's
pause. If power-off or destruction has already made resume impossible, cleanup
logs the failure and ends without changing that transition. Intercepts and
parked vCPUs are cleaned up first.

## vCPU count

`VMI_MSG_VCPU_COUNT` returns the VM's number of vCPUs, fixed for the VM's
lifetime. vCPUs are numbered from 0.

Request body: empty (anything else: `VERR_INVALID_PARAMETER`, connection kept).

Reply body (4 bytes):

| Offset | Size | Field   | Meaning |
|-------:|-----:|---------|---------|
| 0      | 4    | `cCpus` | Number of vCPUs, at least 1. |

## Registers

`VMI_MSG_REGS_READ` reads and `VMI_MSG_REGS_WRITE` writes the registers of one
x86 vCPU, all of them in one request (`VMIX86REGS`, below).

Request body: `VMIVCPUREQ` (8 bytes), followed for `VMI_MSG_REGS_WRITE` by a
`VMIX86REGS` (400 bytes in all):

| Offset | Size | Field         | Meaning |
|-------:|-----:|---------------|---------|
| 0      | 4    | `idCpu`       | vCPU number, below the `VMI_MSG_VCPU_COUNT` count. |
| 4      | 4    | `u32Reserved` | 0. |

Reply body: `VMIX86REGS` for `VMI_MSG_REGS_READ`, empty for `VMI_MSG_REGS_WRITE`.

`VMIX86REGS` (392 bytes), every field a little endian `uint64_t` unless noted:

| Offset | Size | Field | Meaning |
|-------:|-----:|-------|---------|
| 0      | 128  | `aGRegs[16]` | rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8 to r15 (instruction encoding order) |
| 128    | 8    | `rip` | |
| 136    | 8    | `rflags` | the architectural bits |
| 144    | 8    | `cr0` | |
| 152    | 8    | `cr2` | |
| 160    | 8    | `cr3` | |
| 168    | 8    | `cr4` | |
| 176    | 8    | `msrEFER` | MSR `0xc0000080` |
| 184    | 8    | `msrSTAR` | MSR `0xc0000081` |
| 192    | 8    | `msrLSTAR` | MSR `0xc0000082` |
| 200    | 8    | `msrSysEnterCs` | MSR `0x174` |
| 208    | 8    | `msrSysEnterEsp` | MSR `0x175` |
| 216    | 8    | `msrSysEnterEip` | MSR `0x176` |
| 224    | 8    | `msrApicBase` | MSR `0x1b` |
| 232    | 128  | `es`, `cs`, `ss`, `ds`, `fs`, `gs`, `ldtr`, `tr` | `VMIX86SEG` each |
| 360    | 16   | `gdtr` | `VMIX86DTR` |
| 376    | 16   | `idtr` | `VMIX86DTR` |

`VMIX86SEG` (16 bytes): `u64Base` (8), `u32Limit` (4, in bytes, granularity
applied), `Sel` (2, the selector), `u16Reserved` (2, 0). `VMIX86DTR` (16 bytes):
`u64Base` (8), `cbLimit` (2), `au16Reserved` (6, 0). Segment attributes are
not part of the protocol. An MSR the guest CPU doesn't have reads as 0.

The request is served on the vCPU's EMT, between two of its guest instructions:
a read is a consistent snapshot of that vCPU and a write changes all its
registers at once, also while the VM runs. The VM doesn't have to be paused;
pause it to read or write several vCPUs at the same point, or to keep a vCPU
from running on until the registers have been written.

Writes:

- **All or nothing.** Every value is checked first; if any is refused,
  nothing is written.
- **Only changes.** A register written with its current value is not
  touched, so writing back registers read earlier always succeeds and has no
  side effects (no paging mode change, MSR or APIC reprogramming).
- **Like the CPU would load them.** A changed value is refused if the CPU
  would refuse it: RFLAGS with bit 1 clear or a reserved bit set (bits 3, 5,
  15, 22 to 63); CR0, CR4 or EFER with a reserved bit set (for the guest CPU),
  CR0.PG without CR0.PE, CR0.NW without CR0.CD; EFER.LMA other than CR0.PG and
  EFER.LME, long mode without CR4.PAE, CR4.PCIDE outside long mode; CR3 with a
  bit set above the guest's physical address width; with PAE paging,
  page-directory-pointer entries at CR3 that are not in guest RAM or have
  reserved bits set; an MSR value that `WRMSR` refuses. CR0,
  CR3, CR4 and EFER are checked together, not in an instruction order. Paging
  changes take effect as after `MOV` to CR0/CR3/CR4 (TLB flushed, paging mode
  switched).
- **Segments are raw state.** Selector, base and limit are written as given,
  without a descriptor table load, and the segment's attributes are kept. The
  client is responsible for a consistent state: e.g. a limit that doesn't
  match the granularity attribute can keep the vCPU from running again (guru
  meditation) if it is left so when the vCPU resumes.

Reply `rc`, with the connection kept:

- `VINF_SUCCESS` (0);
- `VERR_INVALID_PARAMETER` (-2): wrong body size, nonzero reserved field, or
  (write) a value the CPU would refuse, see above;
- `VERR_INVALID_CPU_ID` (-1018): no vCPU `idCpu`;
- `VERR_VM_INVALID_VM_STATE` (-1907), write only: the VM is neither running
  nor paused (powering on or off, saving or restoring its state, live
  snapshot, teleporting, ...);
- `VERR_NOT_SUPPORTED` (-37): not an x86 VM;
- another VirtualBox status if the vCPU could not be reached.

## Intercepts and events

`VMI_MSG_INTERCEPT_SET` and `VMI_MSG_INTERCEPT_CLEAR` have a 16-byte
`VMIINTERCEPT` request body: `u32 idCpu` at offset 0, `u32 uKind` at offset
4, `u32 uReg` at offset 8 and a reserved `u32` at offset 12 that must be zero.
The kinds:

| `uKind` | Name | `uReg` |
|---|---|---|
| 1 | `VMI_INTERCEPT_BREAKPOINT` (#BP from an int3) | 0 |
| 2 | `VMI_INTERCEPT_SINGLESTEP` | 0 |
| 3 | `VMI_INTERCEPT_CR_WRITE` | control register: 0, 3 or 4 |
| 4 | `VMI_INTERCEPT_MSR_WRITE` | MSR index, any `u32` |
| 5 | `VMI_INTERCEPT_PAGE_ACCESS` (see Page access restrictions) | 0 |

Each (vCPU, kind, `uReg`) is a separate intercept. Their replies have empty
bodies. Setting an already-set intercept or clearing an unset intercept
succeeds. These commands work while running or paused, including before
replying to that vCPU's event. SET returns `VERR_DBGF_ALREADY_ATTACHED` (-1201)
if the VirtualBox debugger is attached. CLEAR still works. Both return
`VERR_INVALID_PARAMETER` (-2) for a wrong body size, a nonzero reserved field
or a nonzero `uReg` with kinds 1, 2 and 5, `VERR_INVALID_CPU_ID` (-1018) for an
unknown vCPU, or `VERR_NOT_SUPPORTED` (-37) for an unknown kind, a control
register other than 0, 3 and 4, or a non-x86 VM. Single-step, CR, MSR and page
access SET also return -37 unless the VM runs on VT-x (single step: with the
monitor trap flag; page access: with nested paging); AMD-V and NEM stepping
would alter guest-visible debug state, and they lack the per-vCPU exit controls
CR and MSR intercepts need. A vCPU holds at
most 64 MSR intercepts; SET of another one returns `VERR_OUT_OF_RESOURCES`
(-80). While any intercept is set, the VirtualBox debugger cannot attach
(`VERR_DBGF_VMI_INTERCEPTS_ACTIVE`, -1242).

Only the intercepted vCPUs and registers exit: a CR3 intercept turns on
CR3-load exiting, a CR0 or CR4 intercept makes the vCPU's guest/host mask
cover every bit of the register (so writes of bits VirtualBox otherwise leaves
to the guest, such as CR0.WP or CR4.TSD, are reported), and an MSR intercept
clears the write passthrough of that MSR in the vCPU's MSR bitmap (reads and
other MSRs do not exit). Clearing restores what VirtualBox itself uses.

`VMI_MSG_EVENT` is unsolicited from server to client: header `idReq` 0,
`fFlags` 0, `rc` 0; body `VMIEVENT` is 40 bytes for every kind: `u32 idCpu`
at 0, `u32 uKind` at 4, `u64 GCPhys` at 8, `u32 cbInstr` at 16, `u32 uReg` at
20, `u64 uOld` at 24 and `u64 uNew` at 32. Events from different vCPUs are
sent in occurrence order. Each vCPU has at most one outstanding event; the
others continue executing.

- Breakpoint event (kind 1): `GCPhys` is the guest-physical address of the
  int3, `cbInstr` its instruction length. RIP is at the int3. A reply without
  reinjection re-executes from RIP, typically after the client restores the
  original instruction; a reply with reinjection delivers #BP to the guest
  with its return address after the int3. `uReg`, `uOld`, `uNew` are zero.
- Single-step event (kind 2): `GCPhys` is the guest-physical address of the
  next instruction (at RIP); `cbInstr` is zero. Steps occur at instruction
  boundaries including the first instruction of an interrupt or exception
  handler. Interrupts are not masked; an intercepted int3 encountered while
  stepping reports the breakpoint before a step. `uReg`, `uOld`, `uNew` are
  zero.
- If RIP is not mapped by the guest's page tables (e.g. a step onto a page
  the guest has not faulted in yet), `GCPhys` is `0xffffffffffffffff`.
- CR write event (kind 3): `uReg` is 0, 3 or 4; `uOld` is the register's
  value, which it still holds; `uNew` is the value it will hold, i.e. the
  `MOV CRn` source operand (32 bits outside 64-bit code; CR0 with ET set;
  CR3 with bit 63, the PCID no-flush bit, cleared). `GCPhys` and `cbInstr` are
  zero.
- MSR write event (kind 4): `uReg` is the MSR index (ECX); `uOld` is the MSR's
  current value, zero if VirtualBox does not implement the MSR (the WRMSR then
  raises #GP as usual); `uNew` is EDX:EAX. `GCPhys` and `cbInstr` are zero.
- Page access event (kind 5, see Page access restrictions): `GCPhys` is the
  guest-physical address of the access (not page aligned), `uReg` the single
  access attempted (1 read, 2 write, 4 execute), and the 8 bytes at offset 24
  (`uOld`, C name `GCPtr`) its guest-linear address when the CPU reported one,
  else 0. `cbInstr` and `uNew` are zero.

CR and MSR write events are reported before the write takes effect, with RIP
at the `MOV CRn` or `WRMSR`. After a reply the instruction executes once,
unchanged (a client may change its source register first with
`VMI_MSG_REGS_WRITE`). Every write that exits is reported, unchanged values and
unimplemented MSRs included, with these exceptions given by VT-x: a CR0 or CR4
write that leaves the value unchanged does not exit, because VT-x exits on
`MOV CR0`/`MOV CR4` only when a bit in the guest/host mask differs from the
read shadow (CR3 writes always exit); `CLTS` and `LMSW` are not reported.
With single step also set, stepping over an intercepted write reports the CR
or MSR event first and the single-step event after the instruction. Writes
VirtualBox emulates in ring-3 without VT-x (IEM execution) are not reported.

The event's vCPU is **Parked** until a reply, but continues to serve register
requests for itself and VM-wide pause/resume operations. It stays Parked
across pause and resume. Memory operations and requests for other vCPUs work
normally. `VMI_MSG_EVENT_REPLY` has a 16-byte `VMIEVENTREPLY` request body
followed by `cbData` bytes:

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `idCpu` | the Parked vCPU |
| 4 | 4 | `fFlags` | bit 0 `VMI_EVENT_REPLY_F_REINJECT` (breakpoint reinjection), other bits 0 |
| 8 | 4 | `uAction` | 0 `VMI_EVENT_REPLY_CONTINUE`, 1 `VMI_EVENT_REPLY_EMULATE`, 2 `VMI_EVENT_REPLY_EMULATE_READ_DATA`, 3 `VMI_EVENT_REPLY_EMULATE_INSN` |
| 12 | 4 | `cbData` | 0 for actions 0 and 1, 1 to 256 for 2, exactly 16 for 3 |
| 16 | `cbData` | `abData` | read data (2) or instruction bytes (3) |

Continue releases the vCPU as described per event kind (a page access is
retried). The emulation actions run the event's instruction once through
VirtualBox's instruction emulator (IEM) with every page access restriction
ignored, so no second page access event occurs midway:

- `EMULATE` (page access events): the instruction as it is.
- `EMULATE_READ_DATA` (page access events): the instruction's memory reads
  return `abData` instead of guest memory, consumed in order (bytes past its
  end read as zero), like Xen's and KVM's emulation read data.
- `EMULATE_INSN` (breakpoint events): `abData` is executed as the instruction
  at RIP instead of the guest's bytes (the int3); RIP then follows it.

The server answers an emulation reply only after the vCPU ran the instruction
(a pending event delivery the access belonged to is delivered instead). If
IEM cannot run it, `rc` is IEM's failure status (e.g.
`VERR_IEM_INSTR_NOT_IMPLEMENTED`, -5300), and the vCPU stays Parked with its
registers and RAM unchanged, so the client can reply again. Device/MMIO
accesses, writes to RAM with an active WRITE handler, any access to RAM with
an active ALL handler, port I/O, privileged, interrupt and REP-prefixed
instructions are refused with `VERR_NOT_SUPPORTED` (-37) before irreversible
effects; read/fetch accesses to WRITE-handler RAM and all accesses to
inactive-handler RAM can be emulated. Physical handler callbacks may change
device or VM state that the RAM journal cannot restore on later IEM failure;
they are never bypassed to force a successful reply. REP can otherwise finish
only one iteration in IEM. An incomplete IEM status is reported as
`VERR_EM_INTERPRETER` (-1148).
An emulated instruction with single step also set reports a single-step event
after it. Reply body is empty. A reply while paused succeeds and the vCPU
continues after resume (a CR or MSR write executes only then; an emulation
runs at once). Errors leave the vCPU Parked:

- `VERR_INVALID_PARAMETER` (-2): body shorter than 16 bytes, `cbData` not
  matching the body or the action, unknown action or flags, reinjection
  requested for a single-step, CR, MSR or page access event or combined with
  an emulation action, `EMULATE` or `EMULATE_READ_DATA` at an event other than
  a page access, `EMULATE_INSN` at an event other than a breakpoint;
- `VERR_INVALID_CPU_ID` (-1018): unknown vCPU;
- `VERR_INVALID_STATE` (-79): vCPU not Parked, including a second reply;
- an emulation failure, see above.

Disconnect removes every page access restriction, clears every intercept (CR
and MSR ones included, restoring the exit controls and MSR bitmap entries
VirtualBox itself uses), stops stepping, releases all Parked vCPUs without
reinjection (a pending CR or MSR write then executes, a page access is retried
and succeeds), and then releases any client-owned pause. A breakpoint byte the
client left in guest memory is not removed by the server.

## Page access restrictions

`VMI_MSG_PAGE_ACCESS_SET` restricts the accesses of the vCPUs to one 4 KiB
guest RAM page, for the whole VM; `VMI_MSG_PAGE_ACCESS_GET` returns a page's
restriction. Request body `VMIPAGEACCESS` (16 bytes): `u64 GCPhys` at 0 (any
address in the page), `u32 fAccess` at 8 and a reserved `u32` at 12 that must
be zero. `fAccess` is the *allowed* access (as on KVM): bit 0 read
(`VMI_PAGE_ACCESS_R`), bit 1 write (`VMI_PAGE_ACCESS_W`), bit 2 execute
(`VMI_PAGE_ACCESS_X`); 7 removes the restriction. GET takes `fAccess` 0 and
its reply body is a `u32`, the allowed access, 7 for an unrestricted page.
SET's reply body is empty. Both work while running or paused, including
before replying to an event; SET is idempotent.

Only vCPU accesses are restricted: those of VT-x and VirtualBox's emulation of
vCPU instructions (IEM). Device and DMA accesses and `VMI_MSG_PHYS_READ`/`WRITE`
are never restricted. A denied access of a vCPU with the page access intercept
(kind 5) raises a page access event before it takes effect, with RIP at the faulting
instruction (the vCPU is Parked; a Continue retries the access, which faults
again unless the restriction was lifted). A vCPU without the intercept uses
its own EPT view without VMI restrictions, performs the access in hardware
unreported, and keeps running; VirtualBox's own access handlers still apply.
An instruction touching several restricted pages raises one event per retry, in
the CPU's fault order. A page access event comes before the breakpoint event
of an int3 and before the single-step event of the same instruction.

Errors (connection kept):

- `VERR_INVALID_PARAMETER` (-2): wrong body size, nonzero reserved field, bits
  other than 0 to 2 in `fAccess`, write without read (SET), nonzero `fAccess`
  (GET);
- `VERR_NOT_SUPPORTED` (-37): the VM does not run on VT-x with nested paging
  (AMD-V, NEM, non-x86), or SET of execute-only when the CPU's EPT lacks
  execute-only support;
- `VERR_PGM_PHYS_PAGE_RESERVED` (-1618): MMIO (special aliased MMIO
  included), MMIO2, ROM or shadowed ROM page;
- `VERR_PGM_INVALID_GC_PHYSICAL_ADDRESS` (-1605): no memory at the address;
- `VERR_DBGF_ALREADY_ATTACHED` (-1201): SET of a restriction while the
  VirtualBox debugger is attached (removing one still works).

While any restriction exists the VirtualBox debugger cannot attach
(`VERR_DBGF_VMI_INTERCEPTS_ACTIVE`, -1242). Restricting a page inside a
large page splits it into 4 KiB pages; every denied access costs a VM exit.

## Disconnecting

Either side may close the socket at any time. The server then accepts the next
client.

## Changes

- Version 1: transport, header and handshake; physical memory read and write
  and RAM ranges; pause, resume and vCPU count; registers.
- Version 2: per-vCPU breakpoint and VT-x single-step intercepts, ordered
  asynchronous events, parked vCPUs and explicit breakpoint reinjection.
  Version 1 greetings are refused with `VERR_VERSION_MISMATCH` (-11).
- Version 2, extended before release: `VMIINTERCEPT` gains `uReg` (16
  bytes), `VMIEVENT` gains `uReg`, `uOld`, `uNew` (40 bytes); CR0/CR3/CR4 and
  MSR write intercepts and events on VT-x. Clients of the first version 2
  layout must be rebuilt (the version stays 2: the first layout was never
  released).
- Version 2, extended before release: page access restrictions
  (`VMI_MSG_PAGE_ACCESS_SET`/`GET`), the page access intercept and event
  (kind 5) on VT-x, and emulation replies: `VMIEVENTREPLY` gains `uAction`
  and `cbData` (16 bytes plus data, the 8-byte form is refused with -2).
