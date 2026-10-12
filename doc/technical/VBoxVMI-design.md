# VBoxVMI server: design notes

VBoxVMI lets a virtual machine introspection (VMI) tool, such as libmicrovmi,
read and control a running VM's state from another process. This file records
how the server is built and why. The wire protocol is in `VBoxVMI-protocol.md`.

## Where it lives

- `src/VBox/VMM/VMMR3/VMIR3.cpp`, compiled into the VMM library when the build
  has `VBOX_WITH_VMI`. It belongs in the VMM because the commands need
  VMM-internal APIs (PGM physical access, the CPU context on the EMT), which
  neither Main nor the VMM function table expose.
- `VBOX_WITH_VMI` is set for Linux hosts in `Config.kmk` on this branch. It is
  a fork-only branch whose purpose is VBoxVMI, and the server is still off for
  every VM unless that VM's config enables it, so building it by default costs
  nothing at runtime and avoids a special build configuration. The
  implementation is Linux only (`SOCK_SEQPACKET` with `SO_PEERCRED`).
- `include/VBox/vmm/vmi.h` holds the wire structures and the two entry points.

## Lifetime and configuration

- Started in `vmR3CreateU` right after the TCP debugger console
  (`DBGCIoCreate`).
- Stopped in `VMR3Destroy`, on the caller's (non-EMT) thread, right after the
  state changes to Destroying and before the destroy request goes to the
  EMTs. The VM is already off, but the EMTs still serve requests, so a server
  thread waiting for an EMT (registers, pause/resume) gets its answer and
  the disconnect cleanup can still use the EMTs. The later `vmR3Destroy` on
  EMT(0), where the debugger console stops, only covers paths that bypass
  `VMR3Destroy`. A state-change callback on power off was not used: it runs
  on an EMT, which a server thread could be waiting for.
- CFGM node `VMI`, set through extradata `VBoxInternal/VMI/...`:
  - `Enabled` (bool, default false). Off unless enabled per VM.
  - `SocketPath` (string). Main (`Console::i_configGuestDbg`) inserts the
    default `<VirtualBox user home>/vmi/<machine UUID>.sock`; extradata overrides it.
  - Unknown values under `VMI` fail VM start, so that a typo such as
    `VBoxInternal/VMI/Enable` is not silently ignored.
- If VBoxVMI is enabled but the socket cannot be created, the VM fails to start
  with an error naming the path: the user asked for it explicitly. The path
  must be absolute; its directory is created with mode 0700 if missing.
- A socket already at the path is probed with `connect`: only
  `ECONNREFUSED` (nobody listening, e.g. left by a crashed VM process) makes
  it stale, and it is replaced. A live socket (another VM with the same path)
  or any other file is an error, and is left alone.
- The socket is removed when the VM is destroyed, only if we bound it and the
  file at the path still has the device and inode recorded after `bind`.

## Threading

One server thread (`VMI`) owned by the module polls a wakeup pipe, the
listening socket and the connected client. Termination writes to the pipe,
shuts the client socket down (unblocking a send or receive) and waits for the
thread, which disconnects the client. The wait is bounded (15 s); if the
thread does not stop, it is abandoned with its instance data rather than
hanging the VM teardown.

- Every send to a client has a 5 s timeout (`SO_SNDTIMEO`): a client that
  stops reading its replies is dropped instead of blocking the server.
- If `accept` fails for lack of file descriptors or memory, the pending
  connection would wake `poll` again at once, so the server stops polling the
  listening socket for 1 s. Accept errors are logged at most 32 times.
- System calls interrupted by signals (`EINTR`) are retried.

A later command is planned as follows:

- physical memory write: served on the server thread like reads (below).

## Physical memory reads

- Served on the server thread with
  `PGMR3PhysBulkGCPhys2CCPtrReadOnlyExternal`, mapping an all-readable request
  under one PGM lock acquisition. If the bulk map fails, only then does the
  server retry with `PGMR3PhysGCPhys2CCPtrReadOnlyExternal` page by page to
  produce each status. Both APIs run on any thread without an EMT round trip.
- `PGMR3PhysReadExternal` was not used: it hands pages with an access handler
  of type ALL (MMIO) or special aliased MMIO (the APIC access page) to an EMT
  and runs `PGMPhysRead` there, which calls the device's MMIO read handler,
  and it fills unassigned addresses with 0xff without saying so. A read must
  never touch a device and must report what it could not read.
- The read-only mapping function refuses MMIO and special aliased MMIO pages
  (`VERR_PGM_PHYS_PAGE_RESERVED`) and unassigned addresses fail the lookup, so
  readable pages are exactly those of the `IMachineDebugger` memory methods
  (`DBGFR3MemRead`, via `PGMPhysGCPhys2CCPtrReadOnly`, makes the same check):
  RAM, ROM and MMIO2. The special aliased MMIO page is reported unreadable
  rather than read as the zero page it is backed by. Zero (not yet allocated)
  RAM pages are mapped read-only to the shared zero page and read as zeros,
  without allocating them.
- Every page gets a status in the reply and unreadable bytes are zeros, so an
  MMIO hole in a bulk read costs nothing more than the hole.
- Size limit: a reply must fit the 64 KiB message, i.e. 15 full pages per
  request, and the client splits bigger reads. Considered: replies split over
  several records with a "more" flag, or a bigger negotiated message size.
  Both save little. With the bulk PGM mapping implementation, the integration
  benchmark on this development host read every RAM range of a paused 512 MiB
  guest at 1916.1 MiB/s (511 MiB in 0.267 s), and a paused 5 GiB guest at
  2825.7 MiB/s (5119 MiB in 1.812 s), using 1 MiB API reads split into
  15-page wire requests. A page-by-page API caller still pays one round trip
  per page. A multi-record reply would also make the client reassemble and
  keep events (version 2) from interleaving with it, and a record much bigger
  than 64 KiB needs a socket send buffer above the default
  `net.core.wmem_max`. Revisit with measurements.
- `VMI_MSG_RAM_RANGES` uses `PGMR3PhysGetRamBootZeroedRanges`, which lists the
  RAM ranges that are not ad hoc (ROM, MMIO, MMIO2) and merges contiguous ones.

## Pause and resume

- `VMR3Suspend` and `VMR3Resume` with the user reason, called on the server
  thread, the same calls `IConsole::Pause` and `IConsole::Resume` make. Main's
  VM state callback (`Console::i_vmstateChangeCallback`) runs on an EMT
  during the state change and moves the machine to `Paused` or `Running`, so
  `VBoxManage`, the GUI and other API clients see VMI pauses like their own,
  and can undo them. A VMI-only pause flag that Main doesn't know about was
  rejected: the VM would look running while stopped, and a user could not
  resume it.
- Synchronous: both are EMT rendezvous, and `VMR3Suspend` returns after every
  EMT has left guest code and EMT(0) has suspended the devices. The server
  thread blocks for that time; no other request of the client is pending.
- Idempotent and not counted, like `IConsole` but without its "already
  paused" error: the server checks the VM state first and answers success
  without a state change when the VM already is in the target state (or reaches
  it through a racing pause or resume). Other states are refused with
  `VERR_VM_INVALID_VM_STATE` instead of entering the live-save variants
  (`RunningLS`), where Main drives the transitions itself.
- Suspensions the server must not count on are refused with
  `VERR_VM_INVALID_VM_STATE`, checked again after a failed suspend or resume:
  - Main's suspension for a configuration change (`VMSUSPENDREASON_RECONFIG`,
    `Console::i_suspendBeforeConfigChange`), for both commands. Main disables
    its state callback for it, so the VM still shows as running, and resumes
    it when the change is done. A pause would report success for a suspension
    that ends without the client; a resume would run the VM in the middle of
    the change and trip Main's state assertions afterwards. The window is
    short; the client retries.
  - Resume of a VM suspended by the host's power management, like
    `IConsole::Resume`: Main resumes it when the host does.
  - Resume while `VMR3TeleportedAndNotFullyResumedYet`: a teleport source,
    suspended once its state went to the target, which owns the disks from
    then on (only Main resumes it, if the teleport fails), and a teleport
    target before Main's first resume. A paused VM whose teleport failed keeps
    the flag until `VBoxManage controlvm resume` clears it; until then VBoxVMI
    cannot resume it.
- The server checks the VMM state, not Main's machine state: Main suspends
  the VM itself for an offline (non-live) snapshot, and a client resume in the
  short window before the save starts would resume it, failing the snapshot.
  `VBoxManage controlvm resume` refuses that, because Main is not `Paused`
  then. Accepted: the VMM has no view of Main's states, and routing pause
  through Main from the VMM would need a new callback interface.
- The server records the pause it took for the client, exactly, for disconnect
  safety to undo the client's pause and only it. `fPausedByClient` is set when
  the client's pause suspended the VM (a pause that found it suspended already
  is not the client's) and cleared by the client's resume and at disconnect.
  A resume by anyone else ends the client's pause too, even if the VM is
  paused again afterwards: a VM state callback (`VMR3AtStateRegister`, on the
  EMT changing the state) counts the transitions to `Resuming`, the only way
  from `Suspended` back to running, and the pause is still the client's only
  while the count equals the one read before its suspend. A plain flag cleared
  by the callback was rejected: a resume between the suspend's return and
  setting the flag would leave it stale. Reading the count before the suspend,
  a resume in that window can only make the client's pause look like someone
  else's, never the reverse.
- `VMI_MSG_VCPU_COUNT` returns `UVM::cCpus`, fixed at VM creation.

## Registers

- One `VMR3ReqCallWaitU` to the vCPU's EMT per read or write, which reads or
  writes the CPU context (`CPUMCTX`) directly, so a full register set is one
  round trip. Not DBGF: `DBGFR3RegCpuQueryBatch` is compiled out (`#if 0`,
  "rewrite / remove"), and the named register access `IMachineDebugger` uses
  either stops every vCPU (`DBGFR3RegNmQueryAll`, an EMT rendezvous returning
  every register of every vCPU) or takes a request per register, with name
  lookups. A normal request, not a priority one: it is served where the EMT
  processes its forced actions.
- Running on the EMT, a request is served between two guest instructions of
  that vCPU, so a read is a snapshot and a write is atomic for that vCPU, also
  on a running VM. Pausing is therefore not required, unlike for the
  per-register `IMachineDebugger` methods, which are not atomic on a running
  VM; it is only needed for a view across vCPUs. Writes are refused unless the
  VM is running or suspended (checked on the EMT, where it cannot change
  under the request): while it saves, teleports or powers off, the CPU state
  may already be saved.
- The context is imported first (`CPUM_IMPORT_EXTRN_RET` with
  `CPUMCTX_EXTRN_ALL`), for NEM, which may keep parts of it in the
  hypervisor; under HM the whole state is in `CPUMCTX` in ring-3. After a
  write, `HMNotifyGuestCtxChanged` (new) flags `HM_CHANGED_ALL_GUEST`. The
  return to ring-3 sets that flag too, except after a host interrupt
  (`VINF_EM_RAW_INTERRUPT`), after which the EMT may serve the request in
  ring-3 and re-enter the guest with the old VMCS state. NEM exports every
  part of the context that is not marked external.
- Writes follow the guest CPU rather than DBGF's raw stores: a changed value
  the CPU would refuse (`POPF`, `MOV` to CRx, `WRMSR`) is refused, all values
  are checked before anything is written, and the MSRs, which only
  `CPUMSetGuestMsr` can check, are written first and written back if anything
  later fails. The APIC base is the last MSR, as the only one whose old value
  may not be writable back. Unchanged registers are not touched, so writing
  back registers read earlier always succeeds and reprograms nothing.
- CR0, CR3, CR4 and EFER are checked as one state (EFER.LMA against CR0.PG and
  EFER.LME, PAE for long mode), since the client writes them together and no
  instruction order applies. A paging change is propagated like `MOV` to CRx
  in IEM: PAE PDPTEs mapped at the new CR3, `PGMFlushTLB`, the CPUM setters,
  `PGMChangeMode`, and `HMFlushTlb` for the guest TLB. EFER goes through
  `CPUMSetGuestEferMsrNoChecks` (TLB flush, NXE to PGM) after the combined
  check, as `WRMSR` cannot change EFER.LMA.
- Segments are written as raw debugger state, like the `IMachineDebugger`
  register setters: selector, base and limit, the hidden part marked valid
  for the selector, attributes kept, no descriptor table load (the client may
  write a base or limit that its table doesn't hold). GDTR and IDTR go
  through `CPUMSetGuestGDTR`/`CPUMSetGuestIDTR`.
- The wire format has no segment attributes: libmicrovmi, the client it was
  made for, has none, and a client writing back attributes it never read
  would need a read first, a second round trip.
- RFLAGS is the architectural part only: CPUM keeps internal state (interrupt
  shadow, ...) in the upper bits, which reads hide and writes keep.

## Security

- The socket file is 0600 from its creation: Linux creates it with the mode
  of the socket inode (minus the umask), which `fchmod` sets before `bind`.
  There is no chmod by path. The mode is checked after `bind`.
- The socket's directory must be owned by the VM process' effective uid and
  not writable by group or others unless sticky; otherwise the VM fails to
  start. Another user could otherwise replace the socket file. This rules out
  e.g. `/tmp` itself (root-owned) but allows a private directory anywhere.
  The default directory is created 0700.
- The server checks the peer's uid with `SO_PEERCRED` and refuses any uid but
  the VM process' effective uid, root included. Root can still introspect the
  VM by other means; refusing it keeps the rule simple.
- Anyone who can set `VBoxInternal/*` extradata for a VM can already enable
  the debugger console and write guest memory, so the extradata switch is a
  convenience, not a security boundary.

## One client at a time

A second connection is accepted only to be told `VERR_RESOURCE_BUSY` in the
greeting and closed, so the client can report a clear error instead of hanging
in the listen backlog. Two tools sharing a session would corrupt each other's
state (pause ownership, later intercepts).

## Disconnect

`vmiR3Disconnect` is the single place where a client's session ends, whatever
the reason (EOF, socket error, protocol error, handshake failure, VM
termination). It is where the server will undo what the client changed:
resume a VM the client paused (`fPausedByClient`) and remove the client's
intercepts. For now a VM the client paused stays paused, and the release log
says so.

## Why a Unix socket and not shared memory

The predecessor (icebox's FDP) used shared memory. A socket gives connection
lifetime for free (the server sees a crashed client as EOF and can clean up),
access control by file mode and peer credentials, and one ordered channel for
replies and later asynchronous events. Batched requests amortise the
per-message cost; a shared-memory data path can be added over the socket later
if profiling shows the need.

## Why SOCK_SEQPACKET

Messages keep their boundaries, so there is no stream framing or partial-read
handling on either side, and the connection semantics (EOF on close) are those
of a stream socket.
