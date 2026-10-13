/** @file
 * VMI - VBoxVMI, the virtual machine introspection server.
 *
 * Wire format of the VBoxVMI protocol, see doc/technical/VBoxVMI-protocol.md.
 */

/*
 * Copyright (C) 2026 Oracle and/or its affiliates.
 *
 * This file is part of VirtualBox base platform packages, as
 * available from https://www.virtualbox.org.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, in version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses>.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL), a copy of it is provided in the "COPYING.CDDL" file included
 * in the VirtualBox distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 *
 * SPDX-License-Identifier: GPL-3.0-only OR CDDL-1.0
 */

#ifndef VBOX_INCLUDED_vmm_vmi_h
#define VBOX_INCLUDED_vmm_vmi_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

#include <VBox/types.h>
#include <iprt/assertcompile.h>

RT_C_DECLS_BEGIN

/** @defgroup grp_vmi   VBoxVMI - Virtual Machine Introspection Server
 * @ingroup grp_vmm
 *
 * All integers on the wire are little endian.  Every message is one
 * SOCK_SEQPACKET record: a VMIMSGHDR followed by the type specific body.
 * @{
 */

/** Protocol magic, the bytes "BVMI" on the wire. */
#define VMI_MAGIC                   UINT32_C(0x494d5642)
/** Protocol version.  Client and server must match exactly. */
#define VMI_VERSION                 UINT32_C(2)
/** Maximum size of a message, header included. */
#define VMI_MSG_SIZE_MAX            _64K

/** @name Message types (VMIMSGHDR::uType)
 * @{ */
/** Handshake: unsolicited greeting from the server, then client request and
 *  server reply.  Body: VMIHELLO. */
#define VMI_MSG_HELLO               UINT16_C(0x0001)
/** Read guest physical memory.  Request body: an array of VMIPHYSRANGE.
 *  Reply body: an int32_t VBox status per guest page each range overlaps (in
 *  request order), then the bytes of every range (in request order). */
#define VMI_MSG_PHYS_READ           UINT16_C(0x0002)
/** Get the guest RAM ranges.  Request body: empty.
 *  Reply body: an array of VMIRAMRANGE sorted by address. */
#define VMI_MSG_RAM_RANGES          UINT16_C(0x0003)
/** Pause the VM (standard suspend, the VM shows as paused), a no-op if it is
 *  suspended already.  The reply is sent once every vCPU has stopped.
 *  Request and reply body: empty. */
#define VMI_MSG_PAUSE               UINT16_C(0x0004)
/** Resume the VM, a no-op if it is running already, whoever paused it.
 *  Request and reply body: empty. */
#define VMI_MSG_RESUME              UINT16_C(0x0005)
/** Get the number of vCPUs.  Request body: empty.
 *  Reply body: a uint32_t, the vCPU count. */
#define VMI_MSG_VCPU_COUNT          UINT16_C(0x0006)
/** Read the registers of one vCPU.  Request body: VMIVCPUREQ.
 *  Reply body: VMIX86REGS. */
#define VMI_MSG_REGS_READ           UINT16_C(0x0007)
/** Write the registers of one vCPU.  Request body: VMIVCPUREQ followed by
 *  VMIX86REGS.  Reply body: empty. */
#define VMI_MSG_REGS_WRITE          UINT16_C(0x0008)
/** Write guest physical memory.  Request body: a uint32_t range count,
 *  that many VMIPHYSRANGE entries, then the bytes of each range in order.
 *  Reply body: an int32_t VBox status per guest page each range overlaps
 *  (in request order). */
#define VMI_MSG_PHYS_WRITE          UINT16_C(0x0009)
/** Set a per-vCPU intercept (idempotent). Request body: VMIINTERCEPT.
 *  Reply body: empty. */
#define VMI_MSG_INTERCEPT_SET       UINT16_C(0x000a)
/** Clear a per-vCPU intercept (idempotent). Request body: VMIINTERCEPT.
 *  Reply body: empty. */
#define VMI_MSG_INTERCEPT_CLEAR     UINT16_C(0x000b)
/** Release a parked vCPU. Request body: VMIEVENTREPLY. Reply body: empty. */
#define VMI_MSG_EVENT_REPLY         UINT16_C(0x000c)
/** Asynchronous event, server to client, request ID zero. Body: VMIEVENT. */
#define VMI_MSG_EVENT               UINT16_C(0x000d)
/** @} */

/** @name Message flags (VMIMSGHDR::fFlags)
 * @{ */
/** The message is the reply to the request with the same VMIMSGHDR::idReq. */
#define VMI_MSG_F_REPLY             UINT16_C(0x0001)
/** @} */

/**
 * Message header.
 */
typedef struct VMIMSGHDR
{
    /** Size of the message in bytes, header included. */
    uint32_t    cbMsg;
    /** Request ID chosen by the client and echoed in the reply.
     *  Zero in messages that are not replies. */
    uint32_t    idReq;
    /** Message type, VMI_MSG_XXX. */
    uint16_t    uType;
    /** Flags, VMI_MSG_F_XXX. */
    uint16_t    fFlags;
    /** VBox status code: the result in replies and in the greeting, zero
     *  in requests. */
    int32_t     rc;
} VMIMSGHDR;
AssertCompileSize(VMIMSGHDR, 16);

/**
 * Body of the VMI_MSG_HELLO messages.
 */
typedef struct VMIHELLO
{
    /** VMI_MAGIC. */
    uint32_t    u32Magic;
    /** VMI_VERSION of the sender. */
    uint32_t    u32Version;
} VMIHELLO;
AssertCompileSize(VMIHELLO, 8);

/**
 * A guest physical memory range in a VMI_MSG_PHYS_READ or
 * VMI_MSG_PHYS_WRITE request.
 */
typedef struct VMIPHYSRANGE
{
    /** The guest physical address of the first byte. */
    uint64_t    GCPhys;
    /** The number of bytes, nonzero.  The range must not wrap around. */
    uint32_t    cb;
    /** Reserved, zero. */
    uint32_t    u32Reserved;
} VMIPHYSRANGE;
AssertCompileSize(VMIPHYSRANGE, 16);

/**
 * A guest RAM range in a VMI_MSG_RAM_RANGES reply.
 */
typedef struct VMIRAMRANGE
{
    /** The guest physical address of the first byte. */
    uint64_t    GCPhysFirst;
    /** The guest physical address of the last byte. */
    uint64_t    GCPhysLast;
} VMIRAMRANGE;
AssertCompileSize(VMIRAMRANGE, 16);

/**
 * The vCPU a VMI_MSG_REGS_READ or VMI_MSG_REGS_WRITE request is for.
 */
typedef struct VMIVCPUREQ
{
    /** The vCPU ID, less than the VMI_MSG_VCPU_COUNT count. */
    uint32_t    idCpu;
    /** Reserved, zero. */
    uint32_t    u32Reserved;
} VMIVCPUREQ;
AssertCompileSize(VMIVCPUREQ, 8);

/** Intercept and event kind: an int3 (#BP) the vCPU executes. */
#define VMI_INTERCEPT_BREAKPOINT    UINT32_C(1)
/** Intercept and event kind: every instruction boundary of the vCPU. */
#define VMI_INTERCEPT_SINGLESTEP    UINT32_C(2)
/** Intercept and event kind: a write to control register VMIINTERCEPT::uReg
 *  (0, 3 or 4), reported before it takes effect (VT-x only). */
#define VMI_INTERCEPT_CR_WRITE      UINT32_C(3)
/** Intercept and event kind: a WRMSR to MSR VMIINTERCEPT::uReg, reported
 *  before it takes effect (VT-x only). */
#define VMI_INTERCEPT_MSR_WRITE     UINT32_C(4)
/** Maximum number of MSR write intercepts per vCPU. */
#define VMI_MSR_INTERCEPTS_MAX      64
/**
 * Request body of VMI_MSG_INTERCEPT_SET and VMI_MSG_INTERCEPT_CLEAR.
 */
typedef struct VMIINTERCEPT
{
    /** vCPU number, below the VMI_MSG_VCPU_COUNT count. */
    uint32_t idCpu;
    /** The intercept, VMI_INTERCEPT_XXX. */
    uint32_t uKind;
    /** The control register number (VMI_INTERCEPT_CR_WRITE) or MSR index
     *  (VMI_INTERCEPT_MSR_WRITE); zero for the other kinds. */
    uint32_t uReg;
    /** Reserved, zero. */
    uint32_t u32Reserved;
} VMIINTERCEPT;
AssertCompileSize(VMIINTERCEPT, 16);

/** VMIEVENTREPLY::fFlags: deliver the intercepted #BP to the guest (breakpoint events only). */
#define VMI_EVENT_REPLY_F_REINJECT  UINT32_C(1)
/**
 * Request body of VMI_MSG_EVENT_REPLY.
 */
typedef struct VMIEVENTREPLY
{
    /** The parked vCPU. */
    uint32_t idCpu;
    /** VMI_EVENT_REPLY_F_XXX, other bits zero. */
    uint32_t fFlags;
} VMIEVENTREPLY;
AssertCompileSize(VMIEVENTREPLY, 8);

/**
 * Body of VMI_MSG_EVENT.
 */
typedef struct VMIEVENT
{
    /** The vCPU, parked until VMI_MSG_EVENT_REPLY. */
    uint32_t idCpu;
    /** The event, VMI_INTERCEPT_XXX. */
    uint32_t uKind;
    /** Guest-physical address of the instruction at RIP (the int3, or the next
     *  one to step), NIL_RTGCPHYS if RIP is not mapped; zero for CR and MSR
     *  writes. */
    uint64_t GCPhys;
    /** Instruction length of the int3; zero for the other kinds. */
    uint32_t cbInstr;
    /** The control register number or MSR index written; zero for breakpoints
     *  and single steps. */
    uint32_t uReg;
    /** The register's value before the write; zero for breakpoints and single steps. */
    uint64_t uOld;
    /** The value the write stores; zero for breakpoints and single steps. */
    uint64_t uNew;
} VMIEVENT;
AssertCompileSize(VMIEVENT, 40);

/**
 * A segment register in VMIX86REGS: the selector and its hidden base and
 * limit.  The attributes are not part of it, writes leave them unchanged.
 */
typedef struct VMIX86SEG
{
    /** The base address. */
    uint64_t    u64Base;
    /** The limit, in bytes (the granularity applied). */
    uint32_t    u32Limit;
    /** The selector. */
    uint16_t    Sel;
    /** Reserved, zero. */
    uint16_t    u16Reserved;
} VMIX86SEG;
AssertCompileSize(VMIX86SEG, 16);

/**
 * A descriptor table register (GDTR, IDTR) in VMIX86REGS.
 */
typedef struct VMIX86DTR
{
    /** The base address. */
    uint64_t    u64Base;
    /** The limit. */
    uint16_t    cbLimit;
    /** Reserved, zero. */
    uint16_t    au16Reserved[3];
} VMIX86DTR;
AssertCompileSize(VMIX86DTR, 16);

/**
 * The registers of an x86 vCPU, the body of a VMI_MSG_REGS_READ reply and
 * part of a VMI_MSG_REGS_WRITE request.
 */
typedef struct VMIX86REGS
{
    /** The general purpose registers, in instruction encoding order: rax, rcx,
     *  rdx, rbx, rsp, rbp, rsi, rdi, r8 to r15. */
    uint64_t    aGRegs[16];
    uint64_t    rip;
    /** RFLAGS, the architectural bits only. */
    uint64_t    rflags;
    uint64_t    cr0;
    uint64_t    cr2;
    uint64_t    cr3;
    uint64_t    cr4;
    /** MSR_K6_EFER. */
    uint64_t    msrEFER;
    /** MSR_K6_STAR. */
    uint64_t    msrSTAR;
    /** MSR_K8_LSTAR. */
    uint64_t    msrLSTAR;
    /** MSR_IA32_SYSENTER_CS. */
    uint64_t    msrSysEnterCs;
    /** MSR_IA32_SYSENTER_ESP. */
    uint64_t    msrSysEnterEsp;
    /** MSR_IA32_SYSENTER_EIP. */
    uint64_t    msrSysEnterEip;
    /** MSR_IA32_APICBASE. */
    uint64_t    msrApicBase;
    /** The segment registers, in instruction encoding order. */
    VMIX86SEG   es;
    VMIX86SEG   cs;
    VMIX86SEG   ss;
    VMIX86SEG   ds;
    VMIX86SEG   fs;
    VMIX86SEG   gs;
    VMIX86SEG   ldtr;
    VMIX86SEG   tr;
    VMIX86DTR   gdtr;
    VMIX86DTR   idtr;
} VMIX86REGS;
AssertCompileSize(VMIX86REGS, 392);

#ifdef IN_RING3
VMMR3_INT_DECL(int)  VMIR3Create(PUVM pUVM, void **ppvVmi);
VMMR3_INT_DECL(void) VMIR3Terminate(PUVM pUVM, void *pvVmi);
VMMR3_INT_DECL(VBOXSTRICTRC) VMIR3DebugEvent(PVM pVM, PVMCPU pVCpu, uint32_t uKind);
#endif

/** @} */

RT_C_DECLS_END

#endif /* !VBOX_INCLUDED_vmm_vmi_h */
