# VBoxVMI protocol, version 1

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
| 4      | 4    | `idReq`  | Request ID chosen by the client, echoed in the reply. 0 in messages that are not replies (greeting, future events). |
| 8      | 2    | `uType`  | Message type (command or event code). A reply has the type of its request. |
| 10     | 2    | `fFlags` | Bit 0 `VMI_MSG_F_REPLY`: the message is a reply. Other bits are 0. |
| 12     | 4    | `rc`     | VirtualBox status code (`iprt/err.h`, `VBox/err.h`): the result in replies and in the greeting, 0 in requests. |

Requests have `fFlags` and `rc` zero. A request whose header is malformed
(size mismatch, nonzero `fFlags` or `rc`, larger than 64 KiB) is a protocol
error: the server drops the connection without replying.

Client IDs are not interpreted by the server; using a fresh nonzero ID per
request lets replies (and, in a later version, events with `idReq` 0) share the
connection.

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

Any other type is answered with `VERR_NOT_SUPPORTED` (-37) and an empty body;
the connection stays up.

## Handshake

`VMIHELLO` body (8 bytes):

| Offset | Size | Field        | Value |
|-------:|-----:|--------------|-------|
| 0      | 4    | `u32Magic`   | `0x494d5642`, the bytes `"BVMI"` |
| 4      | 4    | `u32Version` | Protocol version of the sender, `1` |

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
logs the failure and ends without changing that transition. Future per-client
intercepts are removed at the same session-cleanup point.

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

## Disconnecting

Either side may close the socket at any time. The server then accepts the next
client.

## Changes

- Version 1: transport, header and handshake; physical memory read and write
  and RAM ranges; pause, resume and vCPU count; registers.
