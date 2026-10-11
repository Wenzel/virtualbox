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
  (`DBGCIoCreate`), and stopped in `vmR3Destroy` next to it.
- CFGM node `VMI`, set through extradata `VBoxInternal/VMI/...`:
  - `Enabled` (bool, default false). Off unless enabled per VM.
  - `SocketPath` (string). Main (`Console::i_configGuestDbg`) inserts the
    default `<VirtualBox user home>/vmi/<machine UUID>.sock`; extradata overrides it.
  - Unknown values under `VMI` fail VM start, so that a typo such as
    `VBoxInternal/VMI/Enable` is not silently ignored.
- If VBoxVMI is enabled but the socket cannot be created, the VM fails to start
  with an error naming the path: the user asked for it explicitly. The parent
  directory is created with mode 0700 if missing. A socket left at the path by
  a crashed VM process is replaced; any other file there is an error.
- The socket is removed when the VM is destroyed.

## Threading

One server thread (`VMI`) owned by the module polls a wakeup pipe, the
listening socket and the connected client. Termination writes to the pipe and
waits for the thread, which disconnects the client.

Later commands are planned as follows:

- physical memory read/write: served on the server thread through PGM's
  external physical access functions, so RAM accesses don't hop to an EMT;
- full register set of one vCPU: one request to that vCPU's EMT reading or
  writing the CPU context directly;
- pause/resume: VirtualBox's standard suspend/resume, visible to the GUI and
  `VBoxManage`.

## Security

- The socket is created with mode 0600 (chmod between `bind` and `listen`, so
  nobody can connect while the umask-derived mode applies) and the default
  directory is 0700.
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
termination). Once commands can change VM state, it undoes them there: it
resumes a VM the client paused and removes the client's intercepts.

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
