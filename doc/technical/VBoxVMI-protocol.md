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

## Disconnecting

Either side may close the socket at any time. The server then accepts the next
client.

## Changes

- Version 1: transport, header and handshake.
