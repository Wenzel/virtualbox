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
- `include/VBox/vmm/vmi.h` holds the wire structures and the entry points.

## Lifetime and configuration

- Started in `vmR3CreateU` right after the TCP debugger console
  (`DBGCIoCreate`).
- Stopped in `VMR3Destroy`, on the caller's (non-EMT) thread, right after the
  state changes to Destroying and before the destroy request goes to the
  EMTs. The VM is already off, but the EMTs still serve requests, so a server
  thread waiting for an EMT (registers, pause/resume) gets its answer. The
  server thread is woken and its client socket is shut down; it performs the
  normal session cleanup and exits before the resume-count state callback is
  deregistered and its instance is freed. If its bounded wait expires, both
  the instance and callback are left in place rather than risking a callback
  use-after-free or hanging teardown. The later `vmR3Destroy` on EMT(0), where
  the debugger console stops, only covers paths that bypass `VMR3Destroy`. A
  state-change callback does not initiate cleanup on `POWERING_OFF`: callbacks
  run on an EMT and may not change VM state, while the server can already be
  waiting for an EMT.
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
listening socket and the connected client. EMTs append events to a
mutex-protected ring (one outstanding event per vCPU) and wake that thread;
only the server thread writes the socket, draining events in occurrence
order. Termination sets a separate flag, writes to the same nonblocking
pipe, shuts the client socket down (unblocking a send or receive) and waits
for the thread. The wait is bounded (15 s); if it does not stop, its
instance is abandoned rather than hanging VM teardown.

- Every send to a client has a 5 s timeout (`SO_SNDTIMEO`): a client that
  stops reading its replies is dropped instead of blocking the server.
- If `accept` fails for lack of file descriptors or memory, the pending
  connection would wake `poll` again at once, so the server stops polling the
  listening socket for 1 s. Accept errors are logged at most 32 times.
- System calls interrupted by signals (`EINTR`) are retried.


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

## Physical memory writes

- `VMI_MSG_PHYS_WRITE` uses a leading 32-bit range count because descriptors
  followed immediately by bytes otherwise have no unambiguous boundary. The
  server validates every descriptor, checks for address wrap and requires the
  exact concatenated data length before the first write. A malformed batch
  changes nothing; valid batches report a status per overlapped 4 KiB page,
  continue past failures, and may be partially applied.
- Served on the server thread using `PGMR3PhysGCPhys2CCPtrExternal` for each
  writable page, releasing its mapping immediately after copying. This API
  replaces shared and zero pages, handles write-monitored pages, and refuses
  MMIO and active handlers. Only RAM pages are eligible; MMIO, MMIO2
  (including aliases), ROM and ROM shadows are refused. Allocation can
  delegate to an EMT, during which the page may change type, be remapped or
  gain a handler. The server holds the PGM lock while
  checking the pinned page is still the page at that address, is writable,
  and has no active handlers, through the copy. A delegated mapping cannot
  bypass a newly installed handler or write to a stale mapping.
- `PGMPhysWrite` on the EMT and `PGMR3PhysWriteExternal` off the EMT were
  rejected despite their handler-aware write paths: both can call MMIO device
  handlers, and `PGMR3PhysWriteExternal` silently skips unassigned physical
  addresses. Writing directly through a read-only mapping or
  `PGMPhysSimpleWriteGCPhys` would bypass access handlers and write-monitor
  conversion. An active handler is thus a failed page, not a fallback write,
  even when its backing is RAM. Pause the VM for consistent multi-page writes;
  batch ordering alone is not a transaction.

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
  the CPU would refuse (`POPF`, `MOV` to CRx, `WRMSR`) is refused, and every
  check without side effects runs before anything is written, including the
  PAE PDPTEs at the new CR3 (read with `PGMPhysSimpleReadGCPhys`, checked with
  `PGMGstArePaePdpesValid`), since mapping them changes CPUM and PGM state.
  Then the MSRs, which only `CPUMSetGuestMsr` can check, are written and
  written back if anything later fails. The APIC base is the last MSR and the
  last write that can be refused: its old value may not be writable back
  (x2APIC to xAPIC), and disabling the APIC resets it. After it only mapping
  the PDPTEs (checked already) can fail. Unchanged registers are not touched,
  so writing back registers read earlier always succeeds and reprograms
  nothing.
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

## Breakpoints, steps and parked vCPUs

The per-vCPU breakpoint intercept and single-step ownership flags live in
the shared DBGFCPU, accessible from ring-0; the two VM-wide intercept counts
share the read-only DBGF mirror used by HM and IEM. A vCPU's SET/CLEAR
executes as an EMT request, serialized with the VirtualBox debugger's attach
rendezvous. Attach refuses a nonzero VMI intercept count, while SET checks
the debugger-attached flag before claiming an intercept. CLEAR is idempotent
and works even while a debugger is attached. A breakpoint intercept count
forces the HM debug loop and #BP exception exits (and the NEM/IEM equivalents)
without changing DBGF's own breakpoint count.

All engines funnel guest #BP through `DBGFTrap03Handler`. A DBGF breakpoint
retains priority; a genuine guest #BP with a VMI intercept instead marks the
vCPU as VMI-owned and returns the normal debug-breakpoint status. EM routes
that status to `VMIR3DebugEvent`, not the unattached DBGF event sink. The EMT
translates its flat PC to guest physical memory and disassembles the int3
for its instruction length, then parks. On reply without reinjection, it
reschedules with RIP unchanged. With reinjection, it asserts a TRPM software
exception carrying the instruction length before rescheduling; HM then
injects #BP with the return address past the instruction.

On VT-x with monitor trap flag, setting single-step arms DBGF's raw
single-instruction machinery for that vCPU. HM uses MTF rather than guest
RFLAGS.TF and does not mask interrupts. EM routes VMI-owned stepped statuses
to the same park. A #BP intercepted during stepping is reported first. AMD-V,
NEM and VT-x without MTF are refused: their step path could modify guest
debug state, which this protocol must not do. Clearing single-step stops
arming it; an already-completed step reported after CLEAR is discarded
instead of sent to an unattached debugger.

The EMT's park loop processes its own and VM-wide requests and rendezvous,
as DBGF does while waiting for a debugger command. The server sends queued
events from its own thread. Replies execute as requests on the Parked EMT,
so register read/write, SET/CLEAR, pause/resume and replying while paused
cannot deadlock. A reply to an unparked vCPU or reinjection on a step
changes nothing. A reset, power-off or destruction ends the park; a pause
followed by resume does not. Disconnect removes page restrictions, clears
every intercept and unblocks each Parked EMT without reinjection before
the existing pause cleanup.

## CR and MSR write intercepts

These drive the vCPU's own VT-x controls (libmicrovmi ADR-0006), not DBGF's
CR-write and WRMSR events: those are a per-VM bitmap without an owner that
the debugger console also flips, the WRMSR one turns MSR bitmaps off so every
MSR access of every vCPU exits, and with EPT CR0.WP and several CR4 bits do
not exit at all.

- State: `HMCPU::Vmi` holds the vCPU's CR intercept bits and up to 64 MSR
  entries (bounded, any index). SET/CLEAR run on the vCPU's EMT like the other
  kinds, count toward the debugger exclusion, and flag `HM_CHANGED_GUEST_CR0`,
  `_CR4` or `_OTHER_MSRS` so ring-0 re-exports before the next VM entry.
- CR3: `vmxHCExportGuestCR0` keeps CR3-load exiting on while the intercept is
  set (VirtualBox drops it under EPT with guest paging).
- CR0/CR4: the export writes an all-ones guest/host mask while the intercept
  is set, the fixed mask otherwise. The read shadow already holds the full
  guest value, and the guest-owned bits only change through guest writes,
  which all exit, so switching masks at export time loses nothing. VT-x exits
  on `MOV CR0/CR4` only when a masked bit differs from the read shadow, so
  unchanged writes stay invisible; `CLTS`/`LMSW` exit but are not reported.
- MSRs: ring-0 (`hmR0VmxVmiSyncMsrBitmap`, during the MSR export) sets the
  write-exit bit of newly intercepted MSRs in the guest VMCS's bitmap,
  remembering whether VirtualBox let the guest write it, and restores that bit
  when the intercept is gone, dropping the entry. `hmR0VmxSetMsrPermission`,
  the only writer of the bitmap, keeps an intercepted MSR's write exiting and
  updates the remembered bit instead, so VirtualBox's own later changes (auto
  load/store MSRs) survive. A cleared MSR's entry stays until that restore,
  which is why the 64 entries count those too. Indexes outside the bitmap
  ranges always exit and need no bitmap change.
- Exit: the MOV-to-CR and WRMSR exit handlers check the intercept before
  emulating, store kind, register and new value in `HMCPU::Vmi` and return
  the debug-breakpoint status, like int3. EM routes it to `VMIR3DebugEvent`,
  which reads the old value from the context (MSR: `CPUMQueryGuestMsr`, zero
  on #GP) and parks.
- After the reply the parked EMT executes the instruction once with
  `IEMExecOne` (from the current RIP and registers, so a client's register
  change applies). Re-entering VT-x would exit again on the same instruction,
  and a one-shot "skip" would race with interrupts delivered on entry. A
  reply while paused waits for the resume before executing. With single step
  set the EMT then returns the stepped status, so the step event follows the
  write event. Should IEM not implement the instruction, a one-shot skip lets
  the VT-x exit handler execute it without a second event.
- Disconnect clears the CR and MSR intercepts on each EMT (ring-0 restores the
  masks, CR3 exiting and bitmap bits at the next entry) and releases a Parked
  vCPU, whose write then executes.
- AMD-V and NEM refuse these intercepts (VT-x only); writes executed by IEM
  in ring-3 (e.g. real mode emulation) and nested-guest (VMX non-root) writes
  are not reported.

## Page access restrictions and emulation replies

Page access SET/GET operates on guest RAM pages only (not MMIO, MMIO2, ROM or
unassigned addresses), returning the allowed access bits; only bits 0 to 2
are valid, and 7 (RWX) is unrestricted. SET runs as an EMT rendezvous: it changes
the denied bits stored in `PGMPAGE`, flushes tracked shadow EPT entries and
all vCPU EPT/IEM TLBs, then returns to the server thread. The page pool's
tracking update disables an active 2 MiB PDE and breaks it into 4 KiB mappings.
Allocation and recheck of large pages refuse a range containing a restriction.
This does not install a competing PGM handler, so VirtualBox's own handlers
keep their priority and continue working on those pages. GET takes the PGM
lock and reads the bits. The same debugger-exclusion counter used by the
intercepts counts every restricted page; the SET rendezvous serializes with
debugger attach, and CLEAR (RWX) works with an attached debugger.

EPT has separate watched and unwatched page-pool trees. Root allocation keys
the view by the vCPU's page-access intercept, and each descendant inherits
its parent's view in the pool cache (also for nested-guest EPT and split large
pages). Thus a vCPU can share pages with other vCPUs in its view, but never
shares writable EPT leaves across views. Setting or clearing the per-vCPU
intercept switches its root on that EMT and flags the VMCS EPTP for re-export;
PGM's pool tracking and invalidation continue to cover both trees. SET/CLEAR
invalidates all vCPU EPT TLBs in the existing rendezvous, and pool flush or
paging-mode changes rebuild roots in their respective views.

Only the watched view masks write/execute permissions according to the denied
bits. An EPT entry without read permission cannot be tracked as present by
PGM, so watched pages with read denied (including execute-only) have no EPT
mapping. The unwatched view keeps VirtualBox's own handlers and normal RAM
permissions, and executes guest instructions in hardware even if IEM does not
implement them. The VT-x EPT violation handler checks attempted access on
watched vCPUs before PGM's existing handler path; an allowed access that EPT
cannot map still uses IEM there. There is no temporary VM-wide permission
lift. `PGMPhysVmiCheckAccess` stores the single attempted access, GPA and
CPU-reported GVA in `PGMCPU::Vmi`, returning the debug-breakpoint status; EM
parks the vCPU and sends event kind 5. The per-vCPU page access intercept is
stored in `PGMCPU::Vmi`, not a physical handler: devices, DMA and VBoxVMI's
own physical reads/writes are untouched. Restrictions are removed before
disconnect releases parked vCPUs, which then retry normally.

IEM has its own direct mappings and TLBs, which would bypass PGM physical
handlers. `PGMPhysIemGCPhys2Ptr` forces restricted pages through IEM's
bounce-buffer path; its physical mapping checks access before performing it.
Cross-page mappings check each page in access order; opcode fetch checks
execute access even when fetching from the slow path. The physical
bounce-buffer path has no guest-linear address to report, so its events carry
GVA 0. IEM TLB flushes after every SET/CLEAR prevent old direct mappings
from bypassing these checks.

The reply request validates its flags, action, length and event kind while
the vCPU remains parked. Emulation runs synchronously on that EMT through
`IEMExecOne` or `IEMExecOneWithPrefetchedByPC` (the 16 supplied instruction
bytes override the int3), with every restriction ignored for that run. A
one-by-one EMT rendezvous stops other vCPUs during emulation and any rollback.
Emulated instruction fetches and data operands use checked bounce buffers,
including opcode fetch. Read data is consumed in order by memory reads, zero-filling
past the supplied length. The reply is sent only after IEM returns; on a
failed or incomplete instruction, the saved CPU context and original RAM
bytes (journaled before each bounce-buffer write) are restored, and the vCPU
stays parked for another reply. With single step also enabled the EMT reports
the step after the emulated instruction.

RAM with an inactive physical handler can be emulated like other RAM. Active
WRITE handlers have no read/execute callbacks, so read and instruction fetch
accesses to those pages also work and retain VirtualBox's handler semantics.
An emulated write to an active WRITE handler or any access to an active ALL
handler is refused before the callback: `PGMPhysWrite` and `PGMPhysRead`
invoke arbitrary callbacks that can mutate VM/device state, which restoring
registers and RAM cannot undo if the instruction later fails. Port I/O,
privileged and interrupt instructions are likewise refused before irreversible
effects. REP-prefixed instructions are refused because IEM may successfully
complete only one iteration, which would unpark midway through the instruction.
These refusals return `VERR_NOT_SUPPORTED` with the vCPU parked. Restrictions
still SET/GET and trap on every RAM page regardless of handler type.

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
state (pause ownership and intercepts).

## Disconnect

`vmiR3Disconnect` ends every accepted client session, whether because of EOF,
a socket or send-timeout error, a malformed request, failed handshake, normal
close, or VM termination. It calls `vmiR3CleanupSession` exactly once before
discarding the session state; that function removes intercepts, stops stepping,
releases Parked vCPUs without reinjection, then handles pause ownership.

If the session may still own its pause, cleanup calls `VMR3ResumeIf`
synchronously. That function gathers every EMT in the same serialized
rendezvous used by `VMR3Resume`; on the first EMT, immediately before changing
`Suspended` to `Resuming`, the VBoxVMI predicate verifies the active ownership
flag, captured and current resume generations, `Suspended` state, `USER`
suspend reason, and teleport restriction. No external resume/pause rendezvous
can interleave between that check and the transition. If ownership changed,
the conditional resume is cancelled; otherwise it returns only after the VM
reaches `Running`. Cleanup then drops ownership, logging any transition error.
A pause that merely found the VM paused is not owned. A resume by any actor
ends ownership, so an external resume followed by an external pause is left
paused when the client disconnects.

## Why a Unix socket and not shared memory

The predecessor (icebox's FDP) used shared memory. A socket gives connection
lifetime for free (the server sees a crashed client as EOF and can clean up),
access control by file mode and peer credentials, and one ordered channel for
replies and asynchronous events. Batched requests amortise the per-message
cost; a shared-memory data path can be added over the socket later if
profiling shows the need.

## Why SOCK_SEQPACKET

Messages keep their boundaries, so there is no stream framing or partial-read
handling on either side, and the connection semantics (EOF on close) are those
of a stream socket.
