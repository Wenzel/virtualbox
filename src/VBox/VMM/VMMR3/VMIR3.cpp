/* $Id$ */
/** @file
 * VMI - VBoxVMI, the virtual machine introspection server.
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
 * SPDX-License-Identifier: GPL-3.0-only
 */

/** @page pg_vmi    VMI - VBoxVMI Server
 *
 * Serves the VBoxVMI protocol to one introspection client at a time over a
 * per-VM Unix domain socket (SOCK_SEQPACKET).  It is off unless the VM config
 * has VMI/Enabled (extradata VBoxInternal/VMI/Enabled), and is started at VM
 * creation like the TCP debugger console.  One thread, owned by this module,
 * accepts connections and serves the connected client.
 *
 * See doc/technical/VBoxVMI-protocol.md and doc/technical/VBoxVMI-design.md.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_VM
#define VMCPU_INCL_CPUM_GST_CTX /* The register commands access the guest context directly. */
#include <VBox/vmm/vmi.h>
#include <VBox/vmm/cfgm.h>
#include <VBox/vmm/cpum.h>
#include <VBox/vmm/em.h>
#include <VBox/vmm/hm.h>
#include <VBox/vmm/iem.h>
#include <VBox/vmm/mm.h>
#include <VBox/vmm/pgm.h>
#include <VBox/vmm/vmm.h>
#include "PGMInternal.h"
#include "DBGFInternal.h"
#include "HMInternal.h"
#include "VMInternal.h"
#include <VBox/vmm/vm.h>
#include <VBox/vmm/trpm.h>
#include <VBox/vmm/uvm.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/dir.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/semaphore.h>
#include <iprt/string.h>
#include <iprt/thread.h>
#include <iprt/time.h>


#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>




/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** A client that doesn't read its replies for this long is dropped. */
#define VMI_SEND_TIMEOUT_MS         5000
/** How long VMIR3Terminate waits for the server thread (longer than a send). */
#define VMI_TERM_TIMEOUT_MS         15000
/** How long the server stops accepting after running out of file descriptors. */
#define VMI_ACCEPT_BACKOFF_MS       1000


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * Event state of one vCPU, changed only on that vCPU's EMT.
 */
typedef struct VMIR3CPU
{
    /** Set while the vCPU waits for the reply to its event. */
    bool        fParked;
    /** Set by the reply: deliver the intercepted #BP to the guest. */
    bool        fReinject;
    /** Set by an emulation reply: the instruction ran, see rcEmulated. */
    bool        fEmulated;
    /** The event's kind, VMI_INTERCEPT_XXX. */
    uint32_t    uKind;
    /** Instruction length of the intercepted int3. */
    uint32_t    cbInstr;
    /** IEM's status for the instruction an emulation reply ran. */
    int32_t     rcEmulated;
} VMIR3CPU;
/** Pointer to the event state of one vCPU. */
typedef VMIR3CPU *PVMIR3CPU;


/**
 * The VBoxVMI server instance.
 */
typedef struct VMIR3
{
    /** The user mode VM handle. */
    PUVM            pUVM;
    /** The server thread. */
    RTTHREAD        hThread;
    /** The listening socket. */
    int             fdListen;
    /** Set once we bound the socket: only then is the file at pszPath ours. */
    bool            fBound;
    /** Device and inode of our socket file, so teardown never removes someone else's. */
    dev_t           devSock;
    ino_t           inoSock;
    /** The connected client, -1 if none.  Changed by the server thread only,
     *  under hMtxClient so that VMIR3Terminate can shut it down. */
    int             fdClient;
    RTSEMFASTMUTEX  hMtxClient;
    /** Protects the pending events and their occurrence ordering. */
    RTSEMFASTMUTEX  hMtxEvents;
    /** Ring of events not yet sent, at most one per vCPU, so cCpus entries. */
    VMIEVENT       *paEvents;
    /** Ring index of the oldest pending event. */
    uint32_t        iEventRead;
    /** Ring index of the next event. */
    uint32_t        iEventWrite;
    /** Number of pending events. */
    uint32_t        cEvents;
    /** Per-vCPU event state, cCpus entries. */
    PVMIR3CPU       paCpus;
    /** Distinguishes an event wakeup from termination on the same pipe. */
    bool volatile   fTerminating;
    /** Pipe for waking the server on an event or termination: [0] read, [1] write. */
    int             afdWakeup[2];
    /** Set when the client completed the handshake. */
    bool volatile   fHandshaked;
    /** Set when a VMI_MSG_PAUSE of the connected client suspended the VM (not
     *  one that found it suspended already).  Cleared by the client's
     *  VMI_MSG_RESUME and when the session ends.  The VM is still suspended by
     *  that pause only while cResumes equals cResumesAtPause, see
     *  vmiR3IsPausedByClient. */
    bool            fPausedByClient;
    /** cResumes read before the client's pause suspended the VM. */
    uint32_t        cResumesAtPause;
    /** Number of resumes (VMSTATE_RESUMING) by anyone, counted by vmiR3AtState
     *  on an EMT. */
    uint32_t volatile cResumes;
    /** Set while vmiR3AtState is registered. */
    bool            fAtStateRegistered;
    /** RTTimeMilliTS until which accepting is suspended (out of descriptors). */
    uint64_t        msAcceptResume;
    /** The socket path (heap). */
    char           *pszPath;
    /** Message buffer, sized so that larger messages are detected (MSG_TRUNC). */
    uint8_t         abMsg[VMI_MSG_SIZE_MAX];
    /** Reply body buffer. */
    uint8_t         abReply[VMI_MSG_SIZE_MAX - sizeof(VMIMSGHDR)];
} VMIR3;
/** Pointer to the VBoxVMI server instance. */
typedef VMIR3 *PVMIR3;


/**
 * Sends a message to a socket.
 *
 * @returns VBox status code, VERR_TIMEOUT if the peer doesn't read.
 * @param   fd          The socket.
 * @param   idReq       The request ID (zero if not a reply).
 * @param   uType       The message type.
 * @param   fFlags      The message flags.
 * @param   rcMsg       The status code to put in the header.
 * @param   pvBody      The body.
 * @param   cbBody      The size of the body.
 */
static int vmiR3Send(int fd, uint32_t idReq, uint16_t uType, uint16_t fFlags, int32_t rcMsg, void const *pvBody, size_t cbBody)
{
    AssertReturn(cbBody <= VMI_MSG_SIZE_MAX - sizeof(VMIMSGHDR), VERR_BUFFER_OVERFLOW);
    VMIMSGHDR Hdr;
    Hdr.cbMsg  = (uint32_t)(sizeof(Hdr) + cbBody);
    Hdr.idReq  = idReq;
    Hdr.uType  = uType;
    Hdr.fFlags = fFlags;
    Hdr.rc     = rcMsg;

    /* Header and body as one record, without copying them together. */
    struct iovec aIov[2];
    aIov[0].iov_base = &Hdr;
    aIov[0].iov_len  = sizeof(Hdr);
    aIov[1].iov_base = (void *)pvBody;
    aIov[1].iov_len  = cbBody;
    struct msghdr Msg;
    RT_ZERO(Msg);
    Msg.msg_iov    = aIov;
    Msg.msg_iovlen = RT_ELEMENTS(aIov);

    ssize_t cbSent;
    do
        cbSent = sendmsg(fd, &Msg, MSG_NOSIGNAL);
    while (cbSent < 0 && errno == EINTR);
    if (cbSent == (ssize_t)Hdr.cbMsg)
        return VINF_SUCCESS;
    if (cbSent >= 0)
        return VERR_NET_PROTOCOL_ERROR;
    if (errno == EAGAIN) /* SO_SNDTIMEO expired (EWOULDBLOCK is the same on Linux) */
    {
        LogRel(("VMI: The client doesn't read its replies\n"));
        return VERR_TIMEOUT;
    }
    return RTErrConvertFromErrno(errno);
}


/**
 * Sends a VMI_MSG_HELLO message carrying our magic and version.
 */
static int vmiR3SendHello(int fd, uint32_t idReq, uint16_t fFlags, int32_t rcMsg)
{
    VMIHELLO Hello;
    Hello.u32Magic   = VMI_MAGIC;
    Hello.u32Version = VMI_VERSION;
    return vmiR3Send(fd, idReq, VMI_MSG_HELLO, fFlags, rcMsg, &Hello, sizeof(Hello));
}


/**
 * @callback_method_impl{FNVMATSTATE, Counts the resumes.}
 *
 * Runs on the EMT changing the state.  The only way from Suspended back to
 * running is Resuming (a reset of a suspended VM leaves it suspended).
 */
static DECLCALLBACK(void) vmiR3AtState(PUVM pUVM, PCVMMR3VTABLE pVMM, VMSTATE enmState, VMSTATE enmOldState, void *pvUser)
{
    RT_NOREF(pUVM, pVMM, enmOldState);
    if (enmState == VMSTATE_RESUMING)
        ASMAtomicIncU32(&((PVMIR3)pvUser)->cResumes);
}


/**
 * Whether the VM is still suspended by the connected client's pause: the
 * client's pause suspended it and nobody resumed it since, even if someone
 * paused it again.
 */
static bool vmiR3IsPausedByClient(PVMIR3 pThis)
{
    return pThis->fPausedByClient
        && ASMAtomicReadU32(&pThis->cResumes) == pThis->cResumesAtPause;
}


/**
 * State captured for the disconnect's conditional resume.
 */
typedef struct VMIR3RESUMECHECK
{
    /** The session being cleaned up. */
    PVMIR3     pThis;
    /** The resume generation owned by its pause. */
    uint32_t   cResumesAtPause;
} VMIR3RESUMECHECK;


/**
 * Verifies pause ownership on the first EMT in VMR3ResumeIf's rendezvous.
 *
 * No other suspend or resume rendezvous can interleave between this check and
 * the transition to Resuming.
 */
static DECLCALLBACK(int) vmiR3ResumeCheckOnEmt(PUVM pUVM, void *pvUser)
{
    VMIR3RESUMECHECK const *pCheck = (VMIR3RESUMECHECK const *)pvUser;
    PVMIR3 const pThis = pCheck->pThis;
    if (   pThis->fPausedByClient
        && pThis->cResumesAtPause == pCheck->cResumesAtPause
        && ASMAtomicReadU32(&pThis->cResumes) == pCheck->cResumesAtPause
        && VMR3GetStateU(pUVM) == VMSTATE_SUSPENDED
        && VMR3GetSuspendReason(pUVM) == VMSUSPENDREASON_USER
        && !VMR3TeleportedAndNotFullyResumedYet(pUVM->pVM))
        return VINF_SUCCESS;
    return VERR_CANCELLED;
}

#ifdef VBOX_VMM_TARGET_X86
/**
 * Changes an intercept on its vCPU's EMT, serialized with debugger attach.
 *
 * CR and MSR write intercepts are VT-x controls of this vCPU (ADR-0006): the
 * HM_CHANGED_XXX flags make ring-0 apply them to the VMCS before the next entry.
 *
 * @returns VBox status code.
 * @param   pVM     The cross context VM structure.
 * @param   uKind   The intercept, VMI_INTERCEPT_XXX (validated).
 * @param   uReg    The control register (0, 3, 4) or MSR index; zero otherwise.
 * @param   fSet    Set or clear.
 */
static DECLCALLBACK(int) vmiR3InterceptOnEmt(PVM pVM, uint32_t uKind, uint32_t uReg, bool fSet)
{
    PVMCPU pVCpu = VMMGetCpu(pVM);
    uint32_t iMsr = 0;
    bool fIsSet;
    switch (uKind)
    {
        case VMI_INTERCEPT_BREAKPOINT: fIsSet = pVCpu->dbgf.s.fVmiBreakpoint; break;
        case VMI_INTERCEPT_SINGLESTEP: fIsSet = pVCpu->dbgf.s.fVmiSingleStep; break;
        case VMI_INTERCEPT_CR_WRITE:   fIsSet = RT_BOOL(pVCpu->hm.s.Vmi.fCrWrites & RT_BIT_32(uReg)); break;
        case VMI_INTERCEPT_PAGE_ACCESS: fIsSet = pVCpu->pgm.s.Vmi.fIntercept; break;
        default:
            while (iMsr < pVCpu->hm.s.Vmi.cMsrs && pVCpu->hm.s.Vmi.aMsrs[iMsr].idMsr != uReg)
                iMsr++;
            fIsSet = iMsr < pVCpu->hm.s.Vmi.cMsrs && (pVCpu->hm.s.Vmi.aMsrs[iMsr].fFlags & HM_VMI_MSR_F_ACTIVE);
            break;
    }
    if (fIsSet == fSet)
        return VINF_SUCCESS;
    if (fSet && pVM->dbgf.s.fAttached)
        return VERR_DBGF_ALREADY_ATTACHED;
    if (   fSet
        && uKind != VMI_INTERCEPT_BREAKPOINT
        && (   !HMR3IsVmxEnabled(pVM->pUVM)
            || (   uKind == VMI_INTERCEPT_SINGLESTEP
                && !(pVM->hm.s.ForR3.vmx.Msrs.ProcCtls.n.allowed1 & VMX_PROC_CTLS_MONITOR_TRAP_FLAG))
            || (uKind == VMI_INTERCEPT_PAGE_ACCESS && !HMR3IsNestedPagingActive(pVM->pUVM))))
        return VERR_NOT_SUPPORTED;
    switch (uKind)
    {
        case VMI_INTERCEPT_BREAKPOINT:
            pVCpu->dbgf.s.fVmiBreakpoint = fSet;
            if (fSet)
                ASMAtomicIncU32(&pVM->dbgf.s.cVmiBreakpoints);
            else
                ASMAtomicDecU32(&pVM->dbgf.s.cVmiBreakpoints);
            break;
        case VMI_INTERCEPT_SINGLESTEP:
            pVCpu->dbgf.s.fVmiSingleStep = fSet;
            pVCpu->dbgf.s.fSingleSteppingRaw = fSet;
            if (fSet)
                pVCpu->dbgf.s.fVmiStepped = true;
            break;
        case VMI_INTERCEPT_CR_WRITE:
            pVCpu->hm.s.Vmi.fCrWrites ^= RT_BIT_32(uReg);
            /* The CR0 export also decides CR3-load exiting. */
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_CR0 | HM_CHANGED_GUEST_CR4);
            break;
        case VMI_INTERCEPT_PAGE_ACCESS:
#ifndef VBOX_WITH_ONLY_PGM_NEM_MODE
        {
            int rc = pgmVmiSwitchEptView(pVCpu, fSet);
            if (RT_FAILURE(rc))
                return rc;
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_CR3);
            break;
        }
#else
            pVCpu->pgm.s.Vmi.fIntercept = fSet;
            break;
#endif
        default:
            if (fSet)
            {
                if (iMsr == pVCpu->hm.s.Vmi.cMsrs)
                {
                    /* Entries of cleared intercepts count until ring-0 restored their bitmap bit. */
                    if (iMsr >= RT_ELEMENTS(pVCpu->hm.s.Vmi.aMsrs))
                        return VERR_OUT_OF_RESOURCES;
                    pVCpu->hm.s.Vmi.aMsrs[iMsr].idMsr  = uReg;
                    pVCpu->hm.s.Vmi.aMsrs[iMsr].fFlags = 0;
                    pVCpu->hm.s.Vmi.cMsrs++;
                }
                pVCpu->hm.s.Vmi.aMsrs[iMsr].fFlags |= HM_VMI_MSR_F_ACTIVE;
            }
            else
                pVCpu->hm.s.Vmi.aMsrs[iMsr].fFlags &= ~HM_VMI_MSR_F_ACTIVE; /* Ring-0 restores the bitmap, drops the entry. */
            ASMAtomicUoOrU64(&pVCpu->hm.s.fCtxChanged, HM_CHANGED_GUEST_OTHER_MSRS);
            break;
    }
    if (fSet)
        ASMAtomicIncU32(&pVM->dbgf.s.cVmiIntercepts);
    else
        ASMAtomicDecU32(&pVM->dbgf.s.cVmiIntercepts);
    return VINF_SUCCESS;
}


/**
 * A VMI_MSG_PAGE_ACCESS_SET request for vmiR3PageAccessSetRendezvous, or the
 * removal of every restriction (GCPhys NIL_RTGCPHYS).
 */
typedef struct VMIR3PAGEACCESSSET
{
    RTGCPHYS    GCPhys;
    /** The allowed VMI_PAGE_ACCESS_XXX. */
    uint32_t    fAllowed;
    /** The result. */
    int         rc;
} VMIR3PAGEACCESSSET;


/**
 * Looks up the RAM page of a page access request, PGM lock held.
 *
 * @returns VBox status code: VERR_PGM_INVALID_GC_PHYSICAL_ADDRESS without
 *          memory, VERR_PGM_PHYS_PAGE_RESERVED for MMIO, MMIO2 and ROM.
 */
static int vmiR3PageAccessLookup(PVM pVM, RTGCPHYS GCPhys, PPGMPAGE *ppPage)
{
    *ppPage = pgmPhysGetPageSlow(pVM, GCPhys);
    if (!*ppPage)
        return VERR_PGM_INVALID_GC_PHYSICAL_ADDRESS;
    if (PGM_PAGE_GET_TYPE(*ppPage) != PGMPAGETYPE_RAM)
        return VERR_PGM_PHYS_PAGE_RESERVED;
    return VINF_SUCCESS;
}


/**
 * Changes the restriction of one page, PGM lock held, in the rendezvous.
 *
 * Drops the page's shadow EPT entries (breaking up a large page), so that the
 * next access maps it with the new permissions (see SyncPageWorker).
 *
 * @returns true if the shadow page pool must be cleared (untracked mappings,
 *          e.g. of the zero page).
 */
static bool vmiR3PageAccessChange(PVM pVM, RTGCPHYS GCPhysPage, PPGMPAGE pPage, uint32_t fDenied)
{
    uint32_t const fOld = PGM_PAGE_GET_VMI_DENIED(pPage);
    PGM_PAGE_SET_VMI_DENIED(pPage, fDenied);
    if (!fOld)
    {
        pVM->pgm.s.cVmiRestrictedPages++;
        ASMAtomicIncU32(&pVM->dbgf.s.cVmiIntercepts); /* Debugger exclusion, like intercepts. */
    }
    else if (!fDenied)
    {
        pVM->pgm.s.cVmiRestrictedPages--;
        ASMAtomicDecU32(&pVM->dbgf.s.cVmiIntercepts);
    }
    bool fFlushTlbs = false;
    int rc = pgmPoolTrackUpdateGCPhys(pVM, GCPhysPage, pPage, true /*fFlushPTEs*/, &fFlushTlbs);
    AssertLogRelMsg(rc == VINF_SUCCESS || rc == VINF_PGM_SYNC_CR3, ("%Rrc\n", rc));
    return rc != VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNVMMEMTRENDEZVOUS,
 *      Sets a page access restriction or removes all of them.}
 *
 * A rendezvous, so that no vCPU runs with stale EPT entries or IEM TLBs once
 * the request is answered, and serialized with debugger attach.
 */
static DECLCALLBACK(VBOXSTRICTRC) vmiR3PageAccessSetRendezvous(PVM pVM, PVMCPU pVCpu, void *pvUser)
{
    VMIR3PAGEACCESSSET *pReq = (VMIR3PAGEACCESSSET *)pvUser;
    uint32_t const fDenied = VMI_PAGE_ACCESS_RWX & ~pReq->fAllowed;
    bool fClearPool = false;
    bool fChanged   = false;
    PGM_LOCK_VOID(pVM);
    if (pReq->GCPhys != NIL_RTGCPHYS)
    {
        PPGMPAGE pPage;
        pReq->rc = vmiR3PageAccessLookup(pVM, pReq->GCPhys, &pPage);
        if (RT_SUCCESS(pReq->rc) && PGM_PAGE_GET_VMI_DENIED(pPage) != fDenied)
        {
            if (fDenied && pVM->dbgf.s.fAttached)
                pReq->rc = VERR_DBGF_ALREADY_ATTACHED;
            else
            {
                fClearPool = vmiR3PageAccessChange(pVM, pReq->GCPhys & ~(RTGCPHYS)GUEST_PAGE_OFFSET_MASK, pPage, fDenied);
                fChanged   = true;
            }
        }
    }
    else
    {
        uint32_t const cLookupEntries = RT_MIN(pVM->pgm.s.RamRangeUnion.cLookupEntries, RT_ELEMENTS(pVM->pgm.s.aRamRangeLookup));
        for (uint32_t idxLookup = 0; idxLookup < cLookupEntries; idxLookup++)
        {
            uint32_t const idRamRange = PGMRAMRANGELOOKUPENTRY_GET_ID(pVM->pgm.s.aRamRangeLookup[idxLookup]);
            AssertContinue(idRamRange < RT_ELEMENTS(pVM->pgm.s.apRamRanges));
            PPGMRAMRANGE const pRam = pVM->pgm.s.apRamRanges[idRamRange];
            AssertContinue(pRam);
            for (uint32_t iPage = 0; iPage < (pRam->cb >> GUEST_PAGE_SHIFT); iPage++)
                if (PGM_PAGE_GET_VMI_DENIED(&pRam->aPages[iPage]))
                {
                    fClearPool |= vmiR3PageAccessChange(pVM, pRam->GCPhys + ((RTGCPHYS)iPage << GUEST_PAGE_SHIFT),
                                                        &pRam->aPages[iPage], 0);
                    fChanged = true;
                }
        }
        /* Restrictions of pages PGM re-initialised (e.g. mapped MMIO2 over): nothing left to undo. */
        ASMAtomicSubU32(&pVM->dbgf.s.cVmiIntercepts, pVM->pgm.s.cVmiRestrictedPages);
        pVM->pgm.s.cVmiRestrictedPages = 0;
        pReq->rc = VINF_SUCCESS;
    }
    if (fClearPool)
        pgmR3PoolClearAllRendezvous(pVM, pVCpu, NULL);
    if (fChanged)
    {
        PGM_INVL_ALL_VCPU_TLBS(pVM);
        IEMTlbInvalidateAllPhysicalAllCpus(pVM, pVCpu->idCpu, IEMTLBPHYSFLUSHREASON_MISC);
    }
    PGM_UNLOCK(pVM);
    return VINF_SUCCESS;
}
#endif /* VBOX_VMM_TARGET_X86 */


/** Releases a parked EMT and removes its intercepts during disconnect. */
static DECLCALLBACK(int) vmiR3ClearOnEmt(PVM pVM, PVMIR3 pThis)
{
#ifdef VBOX_VMM_TARGET_X86
    PVMCPU pVCpu = VMMGetCpu(pVM);
    vmiR3InterceptOnEmt(pVM, VMI_INTERCEPT_BREAKPOINT, 0, false);
    vmiR3InterceptOnEmt(pVM, VMI_INTERCEPT_SINGLESTEP, 0, false);
    vmiR3InterceptOnEmt(pVM, VMI_INTERCEPT_CR_WRITE, 0, false);
    vmiR3InterceptOnEmt(pVM, VMI_INTERCEPT_CR_WRITE, 3, false);
    vmiR3InterceptOnEmt(pVM, VMI_INTERCEPT_CR_WRITE, 4, false);
    for (uint32_t i = 0; i < pVCpu->hm.s.Vmi.cMsrs; i++) /* Clearing keeps the entries, see above. */
        vmiR3InterceptOnEmt(pVM, VMI_INTERCEPT_MSR_WRITE, pVCpu->hm.s.Vmi.aMsrs[i].idMsr, false);
    vmiR3InterceptOnEmt(pVM, VMI_INTERCEPT_PAGE_ACCESS, 0, false);
    pVCpu->hm.s.Vmi.fSkipHit = false;
    pThis->paCpus[pVCpu->idCpu].fParked = false;
#else
    RT_NOREF(pVM, pThis);
#endif
    return VINF_SUCCESS;
}


/**
 * Removes page access restrictions and intercepts, releases parked vCPUs (a
 * page access retried then succeeds), then undoes the client's pause.
 */
static void vmiR3CleanupSession(PVMIR3 pThis)
{
#ifdef VBOX_VMM_TARGET_X86
    PVM const pVM = pThis->pUVM->pVM;
    if (pVM->pgm.s.cVmiRestrictedPages && VMR3GetStateU(pThis->pUVM) < VMSTATE_POWERING_OFF)
    {
        VMIR3PAGEACCESSSET Req = { NIL_RTGCPHYS, VMI_PAGE_ACCESS_RWX, VINF_SUCCESS };
        int rc = VMMR3EmtRendezvous(pVM, VMMEMTRENDEZVOUS_FLAGS_TYPE_ONCE, vmiR3PageAccessSetRendezvous, &Req);
        if (RT_FAILURE(rc))
            LogRel(("VMI: Cannot remove the page access restrictions: %Rrc\n", rc));
    }
#endif
    for (uint32_t idCpu = 0; idCpu < pThis->pUVM->cCpus; idCpu++)
    {
        int rc = VMR3ReqCallWaitU(pThis->pUVM, idCpu, (PFNRT)vmiR3ClearOnEmt, 2, pThis->pUVM->pVM, pThis);
        if (RT_FAILURE(rc))
            LogRel(("VMI: Cannot clean up vCPU %u: %Rrc\n", idCpu, rc));
    }
    RTSemFastMutexRequest(pThis->hMtxEvents);
    pThis->cEvents = 0;
    pThis->iEventRead = pThis->iEventWrite;
    RTSemFastMutexRelease(pThis->hMtxEvents);
    if (vmiR3IsPausedByClient(pThis))
    {
        VMIR3RESUMECHECK Check;
        Check.pThis           = pThis;
        Check.cResumesAtPause = pThis->cResumesAtPause;
        int rc = VMR3ResumeIf(pThis->pUVM, VMRESUMEREASON_USER, vmiR3ResumeCheckOnEmt, &Check);
        if (RT_SUCCESS(rc))
            LogRel(("VMI: Resumed the VM paused by the disconnected client\n"));
        else if (rc != VERR_CANCELLED)
            LogRel(("VMI: Cannot resume the VM paused by the disconnected client: %Rrc (VM state %s)\n",
                    rc, VMR3GetStateName(VMR3GetStateU(pThis->pUVM))));
    }
    pThis->fPausedByClient = false;
}


/**
 * Drops the connected client after undoing its session state.
 */
static void vmiR3Disconnect(PVMIR3 pThis)
{
    vmiR3CleanupSession(pThis);
    LogRel(("VMI: Client disconnected\n"));
    RTSemFastMutexRequest(pThis->hMtxClient);
    close(pThis->fdClient);
    pThis->fdClient = -1;
    RTSemFastMutexRelease(pThis->hMtxClient);
    ASMAtomicWriteBool(&pThis->fHandshaked, false);
}


/**
 * Accepts a pending connection: it becomes the client if it is the only one
 * and runs as our user, otherwise it is told why and dropped.
 */
static void vmiR3Accept(PVMIR3 pThis)
{
    int fd;
    do
        fd = accept4(pThis->fdListen, NULL, NULL, SOCK_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0)
    {
        int const iErr = errno;
        /* The connection stays pending, so poll would report it again at once: back off. */
        if (iErr == EMFILE || iErr == ENFILE || iErr == ENOBUFS || iErr == ENOMEM)
            pThis->msAcceptResume = RTTimeMilliTS() + VMI_ACCEPT_BACKOFF_MS;
        LogRelMax(32, ("VMI: accept failed: errno=%d\n", iErr));
        return;
    }

    /* Bound every send, so a client that doesn't read cannot block the server. */
    struct timeval Tv;
    Tv.tv_sec  = VMI_SEND_TIMEOUT_MS / 1000;
    Tv.tv_usec = (VMI_SEND_TIMEOUT_MS % 1000) * 1000;
    int rcGreeting = VINF_SUCCESS;
    struct ucred Cred;
    RT_ZERO(Cred);
    socklen_t    cbCred = sizeof(Cred);
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &Tv, sizeof(Tv)) != 0)
    {
        close(fd);
        return;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &Cred, &cbCred) != 0)
        rcGreeting = RTErrConvertFromErrno(errno);
    else if (Cred.uid != geteuid())
    {
        LogRelMax(32, ("VMI: Refused client pid %d: uid %u is not ours\n", Cred.pid, Cred.uid));
        rcGreeting = VERR_ACCESS_DENIED;
    }
    else if (pThis->fdClient >= 0)
    {
        LogRelMax(32, ("VMI: Refused client pid %d: another client is connected\n", Cred.pid));
        rcGreeting = VERR_RESOURCE_BUSY;
    }

    int rc = vmiR3SendHello(fd, 0 /*idReq*/, 0 /*fFlags*/, rcGreeting);
    if (RT_SUCCESS(rc) && RT_SUCCESS(rcGreeting))
    {
        LogRel(("VMI: Client pid %d connected\n", Cred.pid));
        RTSemFastMutexRequest(pThis->hMtxClient);
        pThis->fdClient = fd;
        RTSemFastMutexRelease(pThis->hMtxClient);
        ASMAtomicWriteBool(&pThis->fHandshaked, false);
    }
    else
        close(fd);
}


/**
 * Handles a VMI_MSG_HELLO request.
 *
 * @returns false if the client must be disconnected.
 */
static bool vmiR3HandleHello(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    if (pThis->fHandshaked)
        return RT_SUCCESS(vmiR3SendHello(pThis->fdClient, pHdr->idReq, VMI_MSG_F_REPLY, VERR_WRONG_ORDER));
    if (cbBody != sizeof(VMIHELLO))
    {
        LogRel(("VMI: Protocol error: hello body of %zu bytes\n", cbBody));
        return false;
    }

    VMIHELLO Hello;
    memcpy(&Hello, (uint8_t const *)pHdr + sizeof(*pHdr), sizeof(Hello));
    int rcReply = VINF_SUCCESS;
    if (Hello.u32Magic != VMI_MAGIC)
        rcReply = VERR_INVALID_MAGIC;
    else if (Hello.u32Version != VMI_VERSION)
        rcReply = VERR_VERSION_MISMATCH;
    LogRel(("VMI: Handshake: client magic %#x version %u: %Rrc\n", Hello.u32Magic, Hello.u32Version, rcReply));

    int rc = vmiR3SendHello(pThis->fdClient, pHdr->idReq, VMI_MSG_F_REPLY, rcReply);
    ASMAtomicWriteBool(&pThis->fHandshaked, RT_SUCCESS(rc) && RT_SUCCESS(rcReply));
    return pThis->fHandshaked;
}


/**
 * Handles a VMI_MSG_PHYS_READ request.
 *
 * Runs on the server thread.  The common all-RAM case is mapped under one PGM
 * lock acquisition.  If that fails (for example, because the batch includes
 * MMIO or an unassigned page), pages are mapped separately to retain a status
 * for each page.  Neither read-only mapping API delegates to an EMT.  MMIO and
 * special aliased MMIO pages are refused without touching the device.
 *
 * @returns false if the client must be disconnected.
 */
static bool vmiR3HandlePhysRead(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    uint8_t const *pbRanges = (uint8_t const *)pHdr + sizeof(*pHdr);
    size_t const   cRanges  = cbBody / sizeof(VMIPHYSRANGE);

    /* Validate every range and size the reply before doing any PGM work. */
    int    rc     = cRanges && cbBody % sizeof(VMIPHYSRANGE) == 0 ? VINF_SUCCESS : VERR_INVALID_PARAMETER;
    size_t cPages = 0;
    size_t cbData = 0;
    for (size_t i = 0; i < cRanges && RT_SUCCESS(rc); i++)
    {
        VMIPHYSRANGE Range;
        memcpy(&Range, &pbRanges[i * sizeof(Range)], sizeof(Range));
        if (   !Range.cb
            || Range.u32Reserved
            || Range.GCPhys + Range.cb - 1 < Range.GCPhys)
            rc = VERR_INVALID_PARAMETER;
        else
        {
            cPages += ((Range.GCPhys + Range.cb - 1) >> GUEST_PAGE_SHIFT) - (Range.GCPhys >> GUEST_PAGE_SHIFT) + 1;
            cbData += Range.cb;
        }
    }
    if (RT_SUCCESS(rc) && cPages * sizeof(int32_t) + cbData > sizeof(pThis->abReply))
        rc = VERR_BUFFER_OVERFLOW;
    if (RT_FAILURE(rc))
        return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, rc, NULL, 0));

    PRTGCPHYS        paGCPhys  = (PRTGCPHYS)RTMemTmpAlloc(cPages * sizeof(*paGCPhys));
    uint16_t         *pacbPages = (uint16_t *)RTMemTmpAlloc(cPages * sizeof(*pacbPages));
    void const      **papvPages = (void const **)RTMemTmpAlloc(cPages * sizeof(*papvPages));
    PPGMPAGEMAPLOCK  paLocks   = (PPGMPAGEMAPLOCK)RTMemTmpAlloc(cPages * sizeof(*paLocks));
    if (!paGCPhys || !pacbPages || !papvPages || !paLocks)
        rc = VERR_NO_TMP_MEMORY;

    size_t iPage = 0;
    for (size_t i = 0; i < cRanges && RT_SUCCESS(rc); i++)
    {
        VMIPHYSRANGE Range;
        memcpy(&Range, &pbRanges[i * sizeof(Range)], sizeof(Range));
        RTGCPHYS GCPhys = Range.GCPhys;
        size_t cbLeft = Range.cb;
        while (cbLeft)
        {
            paGCPhys[iPage] = GCPhys;
            size_t const cb = RT_MIN(cbLeft, GUEST_PAGE_SIZE - (GCPhys & GUEST_PAGE_OFFSET_MASK));
            pacbPages[iPage++] = (uint16_t)cb;
            GCPhys += cb;
            cbLeft -= cb;
        }
    }

    PVM      pVM    = pThis->pUVM->pVM;
    uint8_t *pbStat = pThis->abReply;
    uint8_t *pbData = &pThis->abReply[cPages * sizeof(int32_t)];
    if (RT_SUCCESS(rc))
        rc = PGMR3PhysBulkGCPhys2CCPtrReadOnlyExternal(pVM, (uint32_t)cPages, paGCPhys, papvPages, paLocks);
    if (RT_SUCCESS(rc))
    {
        for (iPage = 0; iPage < cPages; iPage++)
        {
            size_t const cb = pacbPages[iPage];
            int32_t const rcPage = VINF_SUCCESS; /* Normalize informational success statuses. */
            memcpy(pbStat, &rcPage, sizeof(rcPage));
            memcpy(pbData, papvPages[iPage], cb);
            pbStat += sizeof(rcPage);
            pbData += cb;
        }
        PGMPhysBulkReleasePageMappingLocks(pVM, (uint32_t)cPages, paLocks);
    }
    else if (rc != VERR_NO_TMP_MEMORY)
    {
        /* Bulk mapping releases its partial locks on failure.  Retry separately
           only then, so one unreadable page does not fail the request. */
        pbStat = pThis->abReply;
        pbData = &pThis->abReply[cPages * sizeof(int32_t)];
        for (iPage = 0; iPage < cPages; iPage++)
        {
            size_t const cb = pacbPages[iPage];
            void const *pvPage;
            PGMPAGEMAPLOCK Lock;
            int32_t rcPage = PGMR3PhysGCPhys2CCPtrReadOnlyExternal(pVM, paGCPhys[iPage], &pvPage, &Lock);
            if (RT_SUCCESS(rcPage))
            {
                memcpy(pbData, pvPage, cb);
                PGMPhysReleasePageMappingLock(pVM, &Lock);
                rcPage = VINF_SUCCESS;
            }
            else
                memset(pbData, 0, cb);
            memcpy(pbStat, &rcPage, sizeof(rcPage));
            pbStat += sizeof(rcPage);
            pbData += cb;
        }
        rc = VINF_SUCCESS;
    }

    RTMemTmpFree(paLocks);
    RTMemTmpFree(papvPages);
    RTMemTmpFree(pacbPages);
    RTMemTmpFree(paGCPhys);
    if (RT_FAILURE(rc))
        return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, rc, NULL, 0));
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, VINF_SUCCESS,
                                pThis->abReply, (size_t)(pbData - pThis->abReply)));
}


/**
 * Handles a VMI_MSG_PHYS_WRITE request.
 *
 * Validate the entire record before changing guest memory.  Writable PGM
 * mappings account for shared/zero and write-monitored pages; unlike
 * PGMR3PhysWriteExternal they reject MMIO and active access handlers.  Never
 * fall back to that API here: it can invoke a device's MMIO write handler.
 *
 * @returns false if the client must be disconnected.
 */
static bool vmiR3HandlePhysWrite(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    uint8_t const *pbBody = (uint8_t const *)pHdr + sizeof(*pHdr);
    uint32_t cRanges = 0;
    if (cbBody >= sizeof(cRanges))
        memcpy(&cRanges, pbBody, sizeof(cRanges));

    int rc = cbBody >= sizeof(cRanges) && cRanges
           && cRanges <= (cbBody - sizeof(cRanges)) / sizeof(VMIPHYSRANGE)
           ? VINF_SUCCESS : VERR_INVALID_PARAMETER;
    size_t cPages = 0;
    size_t cbData = 0;
    if (RT_SUCCESS(rc))
    {
        for (uint32_t i = 0; i < cRanges; i++)
        {
            VMIPHYSRANGE Range;
            memcpy(&Range, pbBody + sizeof(cRanges) + i * sizeof(Range), sizeof(Range));
            if (   !Range.cb
                || Range.u32Reserved
                || Range.GCPhys + Range.cb - 1 < Range.GCPhys)
            {
                rc = VERR_INVALID_PARAMETER;
                break;
            }
            cPages += ((Range.GCPhys + Range.cb - 1) >> GUEST_PAGE_SHIFT) - (Range.GCPhys >> GUEST_PAGE_SHIFT) + 1;
            cbData += Range.cb;
        }
        if (RT_SUCCESS(rc) && cbData != cbBody - sizeof(cRanges) - cRanges * sizeof(VMIPHYSRANGE))
            rc = VERR_INVALID_PARAMETER;
        if (RT_SUCCESS(rc) && cPages * sizeof(int32_t) > sizeof(pThis->abReply))
            rc = VERR_BUFFER_OVERFLOW;
    }
    if (RT_FAILURE(rc))
        return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, rc, NULL, 0));

    PVM pVM = pThis->pUVM->pVM;
    uint8_t const *pbData = pbBody + sizeof(cRanges) + cRanges * sizeof(VMIPHYSRANGE);
    uint8_t *pbStat = pThis->abReply;
    for (uint32_t i = 0; i < cRanges; i++)
    {
        VMIPHYSRANGE Range;
        memcpy(&Range, pbBody + sizeof(cRanges) + i * sizeof(Range), sizeof(Range));
        RTGCPHYS GCPhys = Range.GCPhys;
        size_t cbLeft = Range.cb;
        while (cbLeft)
        {
            size_t const cb = RT_MIN(cbLeft, GUEST_PAGE_SIZE - (GCPhys & GUEST_PAGE_OFFSET_MASK));
            PGMPAGETYPE const enmType = PGMPhysGetPageType(pVM, GCPhys);
            int32_t rcPage;
            if (enmType == PGMPAGETYPE_INVALID)
                rcPage = VERR_PGM_INVALID_GC_PHYSICAL_ADDRESS;
            else if (enmType != PGMPAGETYPE_RAM)
                rcPage = VERR_PGM_PHYS_PAGE_RESERVED;
            else
            {
                void *pvPage;
                PGMPAGEMAPLOCK Lock;
                rcPage = PGMR3PhysGCPhys2CCPtrExternal(pVM, GCPhys, &pvPage, &Lock);
                if (RT_SUCCESS(rcPage))
                {
                    /* Delegated mappings can race handler installation or a
                       remap; inspect the pinned page under the PGM lock. */
                    PGM_LOCK_VOID(pVM);
                    PPGMPAGE pPage = (PPGMPAGE)(Lock.uPageAndType & ~PGMPAGEMAPLOCK_TYPE_MASK);
                    PPGMPAGE pCurrentPage = pgmPhysGetPageSlow(pVM, GCPhys);
                    PGMPAGETYPE const enmMappedType = (PGMPAGETYPE)PGM_PAGE_GET_TYPE(pPage);
                    if (   pCurrentPage == pPage
                        && enmMappedType == PGMPAGETYPE_RAM
                        && !PGM_PAGE_HAS_ACTIVE_HANDLERS(pPage))
                    {
                        memcpy(pvPage, pbData, cb);
                        rcPage = VINF_SUCCESS;
                    }
                    else
                        rcPage = !pCurrentPage ? VERR_PGM_INVALID_GC_PHYSICAL_ADDRESS : VERR_PGM_PHYS_PAGE_RESERVED;
                    PGM_UNLOCK(pVM);
                    PGMPhysReleasePageMappingLock(pVM, &Lock);
                }
            }
            memcpy(pbStat, &rcPage, sizeof(rcPage));
            pbStat += sizeof(rcPage);
            pbData += cb;
            GCPhys += cb;
            cbLeft -= cb;
        }
    }
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, VINF_SUCCESS,
                                pThis->abReply, (size_t)(pbStat - pThis->abReply)));
}


/**
 * Handles a VMI_MSG_RAM_RANGES request: the PGM RAM ranges that are not ad hoc
 * (ROM, MMIO, MMIO2), contiguous ones merged, sorted by address.
 *
 * @returns false if the client must be disconnected.
 */
static bool vmiR3HandleRamRanges(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    uint32_t const cMax   = sizeof(pThis->abReply) / sizeof(VMIRAMRANGE);
    size_t   const cbList = RT_UOFFSETOF_DYN(PGMPHYSRANGES, aRanges[cMax]);
    PPGMPHYSRANGES pList  = NULL;
    size_t         cbReply = 0;
    int rc = cbBody ? VERR_INVALID_PARAMETER : VINF_SUCCESS;
    if (RT_SUCCESS(rc))
    {
        pList = (PPGMPHYSRANGES)RTMemTmpAlloc(cbList);
        rc = pList ? PGMR3PhysGetRamBootZeroedRanges(pThis->pUVM->pVM, pList, cMax) : VERR_NO_TMP_MEMORY;
    }
    if (RT_SUCCESS(rc))
    {
        /* PGM lists them from the highest address down. */
        for (uint64_t i = 0; i < pList->cRanges; i++)
        {
            PGMPHYSRANGE const *pSrc = &pList->aRanges[pList->cRanges - 1 - i];
            VMIRAMRANGE Range;
            Range.GCPhysFirst = pSrc->GCPhysStart;
            Range.GCPhysLast  = pSrc->GCPhysStart + (pSrc->cPages << GUEST_PAGE_SHIFT) - 1;
            memcpy(&pThis->abReply[i * sizeof(Range)], &Range, sizeof(Range));
        }
        cbReply = pList->cRanges * sizeof(VMIRAMRANGE);
    }
    RTMemTmpFree(pList);
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, rc, pThis->abReply, cbReply));
}


/**
 * Checks whether a pause or resume may be done in the current VM state.
 *
 * @returns VINF_SUCCESS, or VERR_VM_INVALID_VM_STATE if it is refused.
 * @param   pUVM        The user mode VM handle.
 * @param   fPause      Pause (true) or resume (false).
 * @param   pfDone      Set if the VM is in the target state already.
 */
static int vmiR3CheckPauseResume(PUVM pUVM, bool fPause, bool *pfDone)
{
    *pfDone = false;
    VMSTATE const enmState = VMR3GetStateU(pUVM);
    if (enmState == VMSTATE_RUNNING)
    {
        *pfDone = !fPause;
        return VINF_SUCCESS;
    }
    if (enmState != VMSTATE_SUSPENDED)
        return VERR_VM_INVALID_VM_STATE; /* Powering on or off, saving, live snapshot, teleporting, ... */

    /* Main suspends the VM for a configuration change (medium, network) with its
       state callback disabled, so it still shows the VM as running, and resumes
       it when done: neither a pause (Main would resume it) nor a resume (in the
       middle of the change) may count on that suspension. */
    VMSUSPENDREASON const enmReason = VMR3GetSuspendReason(pUVM);
    if (enmReason == VMSUSPENDREASON_RECONFIG)
        return VERR_VM_INVALID_VM_STATE;
    if (fPause)
    {
        *pfDone = true;
        return VINF_SUCCESS;
    }
    /* Like IConsole::Resume: the host resume resumes a VM suspended by the host's
       power management.  A teleport source is suspended with its disks handed
       over to the target: only Main may resume it, if the teleport fails. */
    if (   enmReason == VMSUSPENDREASON_HOST_SUSPEND
        || VMR3TeleportedAndNotFullyResumedYet(pUVM->pVM))
        return VERR_VM_INVALID_VM_STATE;
    return VINF_SUCCESS;
}


/**
 * Handles VMI_MSG_PAUSE and VMI_MSG_RESUME requests.
 *
 * VirtualBox's standard suspend and resume, as IConsole::Pause and
 * IConsole::Resume do it, so Main reports the VM as paused or running by the
 * time the reply is sent.  Synchronous: VMR3Suspend returns once every EMT has
 * stopped executing guest code.  Idempotent: pausing a suspended VM and
 * resuming a running one succeed and do nothing, so one resume undoes any
 * number of pauses, whoever made them.
 *
 * Runs on the server thread, which waits for the EMT rendezvous.
 *
 * @returns false if the client must be disconnected.
 */
static bool vmiR3HandlePauseResume(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    PUVM       pUVM   = pThis->pUVM;
    bool const fPause = pHdr->uType == VMI_MSG_PAUSE;
    bool       fDone  = false;
    int rc = cbBody ? VERR_INVALID_PARAMETER : vmiR3CheckPauseResume(pUVM, fPause, &fDone);
    if (RT_SUCCESS(rc) && !fDone)
    {
        /* Read before suspending: a resume between this and the suspend can only
           make the pause look like someone else's, never the reverse. */
        uint32_t const cResumes = ASMAtomicReadU32(&pThis->cResumes);
        rc = fPause ? VMR3Suspend(pUVM, VMSUSPENDREASON_USER) : VMR3Resume(pUVM, VMRESUMEREASON_USER);
        if (RT_SUCCESS(rc))
        {
            rc = VINF_SUCCESS; /* Not the EM scheduling status. */
            if (fPause)
            {
                pThis->fPausedByClient = true;
                pThis->cResumesAtPause = cResumes;
            }
        }
        else
        {
            /* Someone else may have paused or resumed it meanwhile. */
            int const rc2 = vmiR3CheckPauseResume(pUVM, fPause, &fDone);
            if (RT_FAILURE(rc2) || fDone)
                rc = rc2;
        }
        Log(("VMI: %s the VM for the client: %Rrc\n", fPause ? "Suspending" : "Resuming", rc));
    }
    if (RT_SUCCESS(rc) && !fPause)
        pThis->fPausedByClient = false;
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, rc, NULL, 0));
}


/**
 * Handles a VMI_MSG_VCPU_COUNT request.
 *
 * @returns false if the client must be disconnected.
 */
static bool vmiR3HandleVCpuCount(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    if (cbBody)
        return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, VERR_INVALID_PARAMETER,
                                    NULL, 0));
    uint32_t const cCpus = pThis->pUVM->cCpus;
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, VINF_SUCCESS,
                                &cCpus, sizeof(cCpus)));
}


#ifdef VBOX_VMM_TARGET_X86

/** The MSRs of VMIX86REGS read with CPUMQueryGuestMsr and written with
 *  CPUMSetGuestMsr, in write order.  The APIC base is last: it is the only one
 *  whose old value may not be writable back (APIC mode transitions). */
static uint32_t const g_aidVmiMsrs[] =
{
    MSR_IA32_SYSENTER_CS, MSR_IA32_SYSENTER_ESP, MSR_IA32_SYSENTER_EIP, MSR_K6_STAR, MSR_K8_LSTAR, MSR_IA32_APICBASE
};


/**
 * Gets the g_aidVmiMsrs values of a VMIX86REGS, in the same order.
 */
static void vmiR3RegsGetMsrValues(VMIX86REGS const *pRegs, uint64_t pauValues[RT_ELEMENTS(g_aidVmiMsrs)])
{
    pauValues[0] = pRegs->msrSysEnterCs;
    pauValues[1] = pRegs->msrSysEnterEsp;
    pauValues[2] = pRegs->msrSysEnterEip;
    pauValues[3] = pRegs->msrSTAR;
    pauValues[4] = pRegs->msrLSTAR;
    pauValues[5] = pRegs->msrApicBase;
    AssertCompile(RT_ELEMENTS(g_aidVmiMsrs) == 6);
}


/**
 * Reads a guest MSR, zero if the guest CPU doesn't have it (RDMSR would raise
 * \#GP).
 */
static uint64_t vmiR3QueryMsr(PVMCPU pVCpu, uint32_t idMsr)
{
    uint64_t uValue = 0;
    VBOXSTRICTRC rcStrict = CPUMQueryGuestMsr(pVCpu, idMsr, &uValue);
    return rcStrict == VINF_SUCCESS ? uValue : 0;
}


/**
 * Copies a segment register to its VMIX86SEG.
 */
static void vmiR3SegGet(VMIX86SEG *pDst, CPUMSELREG const *pSrc)
{
    pDst->u64Base  = pSrc->u64Base;
    pDst->u32Limit = pSrc->u32Limit;
    pDst->Sel      = pSrc->Sel;
}


/**
 * Reads the registers of the calling EMT's vCPU, the VMI_MSG_REGS_READ worker.
 *
 * @returns VBox status code.
 * @param   pVM         The cross context VM structure.
 * @param   pRegs       Where to return the registers.
 * @thread  The EMT of the vCPU.
 */
static DECLCALLBACK(int) vmiR3RegsReadOnEmt(PVM pVM, VMIX86REGS *pRegs)
{
    PVMCPU const pVCpu = VMMGetCpu(pVM);
    CPUM_IMPORT_EXTRN_RET(pVCpu, CPUMCTX_EXTRN_ALL);
    PCCPUMCTX const pCtx = &pVCpu->cpum.GstCtx;

    RT_BZERO(pRegs, sizeof(*pRegs));
    for (unsigned i = 0; i < RT_ELEMENTS(pRegs->aGRegs); i++)
        pRegs->aGRegs[i] = pCtx->aGRegs[i].u64;
    pRegs->rip            = pCtx->rip;
    pRegs->rflags         = pCtx->rflags.uBoth & CPUMX86EFLAGS_HW_MASK_64; /* CPUM keeps internal state in the rest. */
    pRegs->cr0            = pCtx->cr0;
    pRegs->cr2            = pCtx->cr2;
    pRegs->cr3            = pCtx->cr3;
    pRegs->cr4            = pCtx->cr4;
    pRegs->msrEFER        = pCtx->msrEFER;
    pRegs->msrSTAR        = vmiR3QueryMsr(pVCpu, MSR_K6_STAR);
    pRegs->msrLSTAR       = vmiR3QueryMsr(pVCpu, MSR_K8_LSTAR);
    pRegs->msrSysEnterCs  = vmiR3QueryMsr(pVCpu, MSR_IA32_SYSENTER_CS);
    pRegs->msrSysEnterEsp = vmiR3QueryMsr(pVCpu, MSR_IA32_SYSENTER_ESP);
    pRegs->msrSysEnterEip = vmiR3QueryMsr(pVCpu, MSR_IA32_SYSENTER_EIP);
    pRegs->msrApicBase    = vmiR3QueryMsr(pVCpu, MSR_IA32_APICBASE);
    vmiR3SegGet(&pRegs->es,   &pCtx->es);
    vmiR3SegGet(&pRegs->cs,   &pCtx->cs);
    vmiR3SegGet(&pRegs->ss,   &pCtx->ss);
    vmiR3SegGet(&pRegs->ds,   &pCtx->ds);
    vmiR3SegGet(&pRegs->fs,   &pCtx->fs);
    vmiR3SegGet(&pRegs->gs,   &pCtx->gs);
    vmiR3SegGet(&pRegs->ldtr, &pCtx->ldtr);
    vmiR3SegGet(&pRegs->tr,   &pCtx->tr);
    pRegs->gdtr.u64Base   = pCtx->gdtr.pGdt;
    pRegs->gdtr.cbLimit   = pCtx->gdtr.cbGdt;
    pRegs->idtr.u64Base   = pCtx->idtr.pIdt;
    pRegs->idtr.cbLimit   = pCtx->idtr.cbIdt;
    return VINF_SUCCESS;
}


/**
 * Checks the reserved fields of a VMIX86REGS.
 */
static bool vmiR3RegsAreReservedZero(VMIX86REGS const *pRegs)
{
    VMIX86SEG const *apSegs[] = { &pRegs->es, &pRegs->cs, &pRegs->ss, &pRegs->ds, &pRegs->fs, &pRegs->gs, &pRegs->ldtr, &pRegs->tr };
    for (unsigned i = 0; i < RT_ELEMENTS(apSegs); i++)
        if (apSegs[i]->u16Reserved)
            return false;
    VMIX86DTR const *apDtrs[] = { &pRegs->gdtr, &pRegs->idtr };
    for (unsigned i = 0; i < RT_ELEMENTS(apDtrs); i++)
        if (apDtrs[i]->au16Reserved[0] || apDtrs[i]->au16Reserved[1] || apDtrs[i]->au16Reserved[2])
            return false;
    return true;
}


/**
 * Checks the values of a VMI_MSG_REGS_WRITE request that the CPU itself would
 * refuse to load (\#GP on POPF, MOV to CRx or WRMSR), checking the control
 * registers and EFER as a whole.  Values equal to the current ones are always
 * accepted, so that registers read earlier can be written back.  No side
 * effects: this runs before anything is written.  The MSRs are checked by
 * CPUMSetGuestMsr when written.
 */
static bool vmiR3RegsAreValid(PVMCPU pVCpu, PCCPUMCTX pCtx, VMIX86REGS const *pNew)
{
    PVM const pVM = pVCpu->CTX_SUFF(pVM);
    if (   pNew->rflags != (pCtx->rflags.uBoth & CPUMX86EFLAGS_HW_MASK_64)
        && (   !(pNew->rflags & X86_EFL_RA1_MASK)
            || (pNew->rflags & ~(uint64_t)(X86_EFL_LIVE_MASK | X86_EFL_RA1_MASK))))
        return false;

    if (   pNew->cr3 != pCtx->cr3
        && (pNew->cr3 & (UINT64_MAX << pVM->cpum.ro.GuestFeatures.cMaxPhysAddrWidth)))
        return false;

    uint64_t const uCr0  = pNew->cr0;
    uint64_t const uCr4  = pNew->cr4;
    uint64_t const uEfer = pNew->msrEFER;
    bool const fModeChanged = uCr0 != pCtx->cr0 || uCr4 != pCtx->cr4 || uEfer != pCtx->msrEFER;
    if (fModeChanged)
    {
        bool const fLma = RT_BOOL(uEfer & MSR_K6_EFER_LMA);
        if (   (uCr0 & ~CPUMGetGuestCR0ValidMask())
            || ((uCr0 & X86_CR0_PG) && !(uCr0 & X86_CR0_PE))
            || ((uCr0 & X86_CR0_NW) && !(uCr0 & X86_CR0_CD))
            || (uCr4 & ~CPUMGetGuestCR4ValidMask(pVM))
            || (uEfer & ~CPUMGetGuestEferMsrValidMask(pVM))
            /* EFER.LMA is CR0.PG and EFER.LME, long mode needs PAE, and PCIDE long mode. */
            || fLma != ((uCr0 & X86_CR0_PG) && (uEfer & MSR_K6_EFER_LME))
            || (fLma && !(uCr4 & X86_CR4_PAE))
            || ((uCr4 & X86_CR4_PCIDE) && !fLma))
            return false;
    }

    /* PAE paging loads the PDPTEs at CR3 (MOV to CR0, CR3, CR4): they must be in
       memory and have no reserved bits set.  Mapping them changes CPUM and PGM
       state, so they are only read and checked here. */
    if (   (fModeChanged || pNew->cr3 != pCtx->cr3)
        && CPUMIsPaePagingEnabled(uCr0, uCr4, uEfer))
    {
        X86PDPE aPdpes[X86_PG_PAE_PDPE_ENTRIES];
        int rc = PGMPhysSimpleReadGCPhys(pVM, aPdpes, pNew->cr3 & X86_CR3_PAE_PAGE_MASK, sizeof(aPdpes));
        if (RT_FAILURE(rc) || !PGMGstArePaePdpesValid(pVCpu, aPdpes))
            return false;
    }
    return true;
}


/**
 * Writes the changed MSRs of a VMI_MSG_REGS_WRITE request in g_aidVmiMsrs order.
 *
 * @returns VBox status code, VERR_INVALID_PARAMETER if the guest CPU refuses a
 *          value (WRMSR would raise \#GP).
 * @param   pVCpu       The cross context virtual CPU structure.
 * @param   pauOld      The current values.
 * @param   pauNew      The new values.
 * @param   pcDone      Where to return how many of them were processed, for
 *                      vmiR3MsrsUndo.
 */
static int vmiR3MsrsSet(PVMCPU pVCpu, uint64_t const *pauOld, uint64_t const *pauNew, size_t *pcDone)
{
    for (size_t i = 0; i < RT_ELEMENTS(g_aidVmiMsrs); i++)
    {
        *pcDone = i;
        if (pauNew[i] == pauOld[i])
            continue;
        VBOXSTRICTRC rcStrict = CPUMSetGuestMsr(pVCpu, g_aidVmiMsrs[i], pauNew[i]);
        if (rcStrict != VINF_SUCCESS)
        {
            if (rcStrict == VERR_CPUM_RAISE_GP_0)
                return VERR_INVALID_PARAMETER;
            return RT_FAILURE(VBOXSTRICTRC_VAL(rcStrict)) ? VBOXSTRICTRC_VAL(rcStrict) : VERR_IPE_UNEXPECTED_INFO_STATUS;
        }
    }
    *pcDone = RT_ELEMENTS(g_aidVmiMsrs);
    return VINF_SUCCESS;
}


/**
 * Writes back the old values of the first @a cMsrs MSRs vmiR3MsrsSet processed.
 */
static void vmiR3MsrsUndo(PVMCPU pVCpu, uint64_t const *pauOld, uint64_t const *pauNew, size_t cMsrs)
{
    while (cMsrs-- > 0)
        if (pauNew[cMsrs] != pauOld[cMsrs])
        {
            VBOXSTRICTRC rcStrict = CPUMSetGuestMsr(pVCpu, g_aidVmiMsrs[cMsrs], pauOld[cMsrs]);
            if (rcStrict != VINF_SUCCESS)
                LogRel(("VMI: Failed to restore MSR %#x of vCPU %u to %#RX64: %Rrc\n",
                        g_aidVmiMsrs[cMsrs], pVCpu->idCpu, pauOld[cMsrs], VBOXSTRICTRC_VAL(rcStrict)));
        }
}


/**
 * Writes a segment register from its VMIX86SEG, if it changed, as raw state:
 * no descriptor table load, the attributes are kept, and the hidden part is
 * marked valid for the new selector.
 */
static void vmiR3SegSet(PVMCPU pVCpu, PCPUMSELREG pSeg, VMIX86SEG const *pNew, uint32_t fChanged)
{
    if (   pSeg->Sel      == pNew->Sel
        && pSeg->u64Base  == pNew->u64Base
        && pSeg->u32Limit == pNew->u32Limit)
        return;
    pSeg->Sel      = pNew->Sel;
    pSeg->ValidSel = pNew->Sel;
    pSeg->fFlags   = CPUMSELREG_FLAGS_VALID;
    pSeg->u64Base  = pNew->u64Base;
    pSeg->u32Limit = pNew->u32Limit;
    CPUMSetChangedFlags(pVCpu, CPUM_CHANGED_HIDDEN_SEL_REGS | fChanged);
}


/**
 * Writes the registers of the calling EMT's vCPU, the VMI_MSG_REGS_WRITE
 * worker.
 *
 * Running on the vCPU's EMT, the write happens between two guest instructions
 * of that vCPU, all registers at once.  Nothing is written if a value is
 * refused.  Registers keeping their value are not touched, so that unchanged
 * control registers and MSRs have no side effects.  Changes to the paging
 * registers are propagated to PGM like MOV to CRx does, and HM reloads the
 * whole state before the next VM-entry.
 *
 * @returns VBox status code.
 * @param   pVM         The cross context VM structure.
 * @param   pNew        The registers to write.
 * @thread  The EMT of the vCPU.
 */
static DECLCALLBACK(int) vmiR3RegsWriteOnEmt(PVM pVM, VMIX86REGS const *pNew)
{
    PVMCPU const  pVCpu    = VMMGetCpu(pVM);
    VMSTATE const enmState = VMR3GetState(pVM);
    if (enmState != VMSTATE_RUNNING && enmState != VMSTATE_SUSPENDED)
        return VERR_VM_INVALID_VM_STATE; /* Saving, teleporting, powering off, ...: the state may be gone already. */
    CPUM_IMPORT_EXTRN_RET(pVCpu, CPUMCTX_EXTRN_ALL);
    PCPUMCTX const pCtx = &pVCpu->cpum.GstCtx;
    if (!vmiR3RegsAreValid(pVCpu, pCtx, pNew))
        return VERR_INVALID_PARAMETER;

    /*
     * What may still fail comes first and is undone on failure: the MSRs, which
     * only WRMSR checks, the APIC base last as it may not be undoable, then
     * mapping the paging structures at the new CR3, before the mode change, like
     * MOV to CRx, which was checked above already.
     */
    uint64_t auOld[RT_ELEMENTS(g_aidVmiMsrs)];
    uint64_t auNew[RT_ELEMENTS(g_aidVmiMsrs)];
    for (size_t i = 0; i < RT_ELEMENTS(g_aidVmiMsrs); i++)
        auOld[i] = vmiR3QueryMsr(pVCpu, g_aidVmiMsrs[i]);
    vmiR3RegsGetMsrValues(pNew, auNew);
    size_t cMsrsDone = 0;
    int rc = vmiR3MsrsSet(pVCpu, auOld, auNew, &cMsrsDone);

    uint64_t const uOldEfer       = pCtx->msrEFER;
    bool const     fModeChanged   = pNew->cr0 != pCtx->cr0 || pNew->cr4 != pCtx->cr4 || pNew->msrEFER != uOldEfer;
    bool const     fPagingChanged = fModeChanged || pNew->cr3 != pCtx->cr3;
    if (RT_SUCCESS(rc) && fPagingChanged && CPUMIsPaePagingEnabled(pNew->cr0, pNew->cr4, pNew->msrEFER))
    {
        rc = PGMGstMapPaePdpesAtCr3(pVCpu, pNew->cr3);
        if (RT_FAILURE(rc)) /* MOV to CRx raises #GP(0); checked above, so unlikely. */
            rc = VERR_INVALID_PARAMETER;
    }
    if (RT_SUCCESS(rc) && fPagingChanged)
        rc = PGMFlushTLB(pVCpu, pNew->cr3, true /*fGlobal*/);
    if (RT_FAILURE(rc))
    {
        vmiR3MsrsUndo(pVCpu, auOld, auNew, cMsrsDone);
        HMNotifyGuestCtxChanged(pVCpu);
        return rc;
    }

    /*
     * The rest cannot fail.
     */
    for (unsigned i = 0; i < RT_ELEMENTS(pNew->aGRegs); i++)
        pCtx->aGRegs[i].u64 = pNew->aGRegs[i];
    pCtx->rip          = pNew->rip;
    pCtx->rflags.uBoth = (pCtx->rflags.uBoth & ~CPUMX86EFLAGS_HW_MASK_64) | pNew->rflags;

    vmiR3SegSet(pVCpu, &pCtx->es,   &pNew->es,   0);
    vmiR3SegSet(pVCpu, &pCtx->cs,   &pNew->cs,   0);
    vmiR3SegSet(pVCpu, &pCtx->ss,   &pNew->ss,   0);
    vmiR3SegSet(pVCpu, &pCtx->ds,   &pNew->ds,   0);
    vmiR3SegSet(pVCpu, &pCtx->fs,   &pNew->fs,   0);
    vmiR3SegSet(pVCpu, &pCtx->gs,   &pNew->gs,   0);
    vmiR3SegSet(pVCpu, &pCtx->ldtr, &pNew->ldtr, CPUM_CHANGED_LDTR);
    vmiR3SegSet(pVCpu, &pCtx->tr,   &pNew->tr,   CPUM_CHANGED_TR);
    if (pNew->gdtr.u64Base != pCtx->gdtr.pGdt || pNew->gdtr.cbLimit != pCtx->gdtr.cbGdt)
        CPUMSetGuestGDTR(pVCpu, pNew->gdtr.u64Base, pNew->gdtr.cbLimit);
    if (pNew->idtr.u64Base != pCtx->idtr.pIdt || pNew->idtr.cbLimit != pCtx->idtr.cbIdt)
        CPUMSetGuestIDTR(pVCpu, pNew->idtr.u64Base, pNew->idtr.cbLimit);

    if (pNew->cr2 != pCtx->cr2)
        CPUMSetGuestCR2(pVCpu, pNew->cr2);
    if (fPagingChanged)
    {
        CPUMSetGuestCR0(pVCpu, pNew->cr0);
        CPUMSetGuestCR4(pVCpu, pNew->cr4);
        CPUMSetGuestCR3(pVCpu, pNew->cr3);
        if (pNew->msrEFER != uOldEfer)
            CPUMSetGuestEferMsrNoChecks(pVCpu, uOldEfer, pNew->msrEFER);
        if (fModeChanged)
        {
            rc = PGMChangeMode(pVCpu, pCtx->cr0, pCtx->cr4, pCtx->msrEFER, false /*fForce*/);
            if (RT_FAILURE(rc))
                LogRel(("VMI: Paging mode change of vCPU %u failed: %Rrc\n", pVCpu->idCpu, rc));
        }
        HMFlushTlb(pVCpu);
    }
    HMNotifyGuestCtxChanged(pVCpu);
    return RT_FAILURE(rc) ? rc : VINF_SUCCESS;
}


/**
 * Handles VMI_MSG_REGS_READ and VMI_MSG_REGS_WRITE requests: one request to the
 * vCPU's EMT, which the server thread waits for.
 *
 * @returns false if the client must be disconnected.
 */
static bool vmiR3HandleRegs(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    PUVM       pUVM   = pThis->pUVM;
    bool const fWrite = pHdr->uType == VMI_MSG_REGS_WRITE;
    VMIVCPUREQ Req;
    VMIX86REGS Regs;
    RT_ZERO(Regs);
    int        rc     = VERR_INVALID_PARAMETER;
    if (cbBody == sizeof(Req) + (fWrite ? sizeof(Regs) : 0))
    {
        uint8_t const *pbBody = (uint8_t const *)pHdr + sizeof(*pHdr);
        memcpy(&Req, pbBody, sizeof(Req));
        if (fWrite)
            memcpy(&Regs, &pbBody[sizeof(Req)], sizeof(Regs));
        if (Req.u32Reserved || (fWrite && !vmiR3RegsAreReservedZero(&Regs)))
            rc = VERR_INVALID_PARAMETER;
        else if (Req.idCpu >= pUVM->cCpus)
            rc = VERR_INVALID_CPU_ID;
        else if (fWrite)
            rc = VMR3ReqCallWaitU(pUVM, Req.idCpu, (PFNRT)vmiR3RegsWriteOnEmt, 2, pUVM->pVM, &Regs);
        else
            rc = VMR3ReqCallWaitU(pUVM, Req.idCpu, (PFNRT)vmiR3RegsReadOnEmt, 2, pUVM->pVM, &Regs);
        Log(("VMI: %s the registers of vCPU %u: %Rrc\n", fWrite ? "Writing" : "Reading", Req.idCpu, rc));
    }
    if (RT_SUCCESS(rc))
        rc = VINF_SUCCESS;
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, rc,
                                &Regs, RT_SUCCESS(rc) && !fWrite ? sizeof(Regs) : 0));
}


/**
 * Runs the instruction of an emulation reply on the parked EMT, once, with
 * every page access restriction ignored.
 *
 * On failure the registers and journaled RAM writes are restored.
 *
 * @returns VBox status code, the failure to report if IEM could not run it.
 * @param   pVCpu       The cross context virtual CPU structure of the calling EMT.
 * @param   uAction     VMI_EVENT_REPLY_EMULATE, _EMULATE_READ_DATA or _EMULATE_INSN.
 * @param   pbData      The read data or instruction bytes.
 * @param   cbData      Their size (validated).
 * @param   prcEmulated Where to store IEM's status on success.
 */
static int vmiR3Emulate(PVMCPU pVCpu, uint32_t uAction, uint8_t const *pbData, uint32_t cbData, int32_t *prcEmulated)
{
    Assert(pVCpu->pgm.s.Vmi.pvJournal == NULL);
    CPUM_IMPORT_EXTRN_RET(pVCpu, CPUMCTX_EXTRN_ALL);
    /* DIS reads the guest opcode before IEM's checked bounce buffers are active. */
    RTGCPTR const GCPtrRip = CPUMGetGuestFlatPC(pVCpu);
    RTGCPHYS GCPhysRip;
    if (   RT_FAILURE(PGMPhysGCPtr2GCPhys(pVCpu, GCPtrRip, &GCPhysRip))
        || !PGMPhysVmiIsEmulationRam(pVCpu->CTX_SUFF(pVM), GCPhysRip, VMI_PAGE_ACCESS_X))
        return VERR_NOT_SUPPORTED;
    if (   uAction != VMI_EVENT_REPLY_EMULATE_INSN
        && (GCPtrRip & GUEST_PAGE_OFFSET_MASK) > GUEST_PAGE_SIZE - 15 /* longest x86 instruction */)
    {
        RTGCPHYS GCPhysNext;
        if (   RT_FAILURE(PGMPhysGCPtr2GCPhys(pVCpu, (GCPtrRip | GUEST_PAGE_OFFSET_MASK) + 1, &GCPhysNext))
            || !PGMPhysVmiIsEmulationRam(pVCpu->CTX_SUFF(pVM), GCPhysNext, VMI_PAGE_ACCESS_X))
            return VERR_NOT_SUPPORTED;
    }
    /*
     * Privileged, port-I/O and interrupt instructions can change device or
     * VM state that a RAM journal cannot restore after an IEM failure.
     * IEM can stop a REP instruction after one iteration, so refuse it
     * rather than unpark halfway through the instruction.
     */
    DISSTATE Dis;
    unsigned cbInstr = 0;
    int rcDis;
    if (uAction == VMI_EVENT_REPLY_EMULATE_INSN)
        rcDis = DISInstrWithPrefetchedBytes((RTUINTPTR)CPUMGetGuestFlatPC(pVCpu), CPUMGetGuestDisMode(pVCpu),
                                           DISOPTYPE_ALL, pbData, cbData, NULL, NULL, &Dis, &cbInstr);
    else
        rcDis = EMInterpretDisasCurrent(pVCpu, &Dis, &cbInstr);
    if (   RT_FAILURE(rcDis)
        || !Dis.pCurInstr
        || (Dis.pCurInstr->fOpType & (  DISOPTYPE_DANGEROUS | DISOPTYPE_PORTIO | DISOPTYPE_PRIVILEGED
                                    | DISOPTYPE_INTERRUPT | DISOPTYPE_INHIBIT_IRQS))
        || (Dis.x86.fPrefix & (DISPREFIX_REP | DISPREFIX_REPNE)))
        return VERR_NOT_SUPPORTED;
    PCPUMCTX pCtxSaved = (PCPUMCTX)RTMemTmpAlloc(sizeof(*pCtxSaved));
    AssertReturn(pCtxSaved, VERR_NO_TMP_MEMORY);
    *pCtxSaved = pVCpu->cpum.GstCtx;

    pVCpu->pgm.s.Vmi.fEmulating = true;
    if (uAction == VMI_EVENT_REPLY_EMULATE_READ_DATA)
    {
        pVCpu->pgm.s.Vmi.pbReadData  = pbData;
        pVCpu->pgm.s.Vmi.offReadData = 0;
        pVCpu->pgm.s.Vmi.cbReadData  = (uint16_t)cbData;
    }
    IEMTlbInvalidateAll(pVCpu); /* Every page must be checked and bounce buffered. */
    VBOXSTRICTRC rcStrict;
    if (uAction == VMI_EVENT_REPLY_EMULATE_INSN)
        rcStrict = IEMExecOneWithPrefetchedByPC(pVCpu, CPUMGetGuestFlatPC(pVCpu), pbData, cbData);
    else if (TRPMHasTrap(pVCpu))
        rcStrict = IEMInjectTrpmEvent(pVCpu); /* The access delivered an interrupt or exception. */
    else
        rcStrict = IEMExecOne(pVCpu);
    pVCpu->pgm.s.Vmi.fEmulating = false;
    pVCpu->pgm.s.Vmi.cbReadData = 0;
    pVCpu->pgm.s.Vmi.pbReadData = NULL;
    bool const fCompleted = rcStrict == VINF_SUCCESS || rcStrict == VINF_EM_RESCHEDULE;
    PGMPhysVmiEndEmulation(pVCpu, fCompleted);
    IEMTlbInvalidateAll(pVCpu);

    int rc = VINF_SUCCESS;
    if (!fCompleted)
    {
        int const rcFailure = RT_FAILURE(VBOXSTRICTRC_VAL(rcStrict)) ? VBOXSTRICTRC_VAL(rcStrict) : VERR_EM_INTERPRETER;
        LogRel(("VMI: Cannot emulate the instruction of vCPU %u at %RGv: %Rrc\n",
                pVCpu->idCpu, CPUMGetGuestFlatPC(pVCpu), rcFailure));
        pVCpu->cpum.GstCtx = *pCtxSaved;
        HMNotifyGuestCtxChanged(pVCpu);
        rc = rcFailure;
    }
    else
        *prcEmulated = VBOXSTRICTRC_VAL(rcStrict);
    RTMemTmpFree(pCtxSaved);
    return rc;
}

/** Data for emulation while the other EMTs wait at the rendezvous. */
typedef struct VMIR3EMULATE
{
    VMCPUID            idCpu;
    uint32_t           uAction;
    uint8_t const     *pbData;
    uint32_t           cbData;
    int32_t           *prcEmulated;
    int                rc;
} VMIR3EMULATE;


static DECLCALLBACK(VBOXSTRICTRC) vmiR3EmulateRendezvous(PVM pVM, PVMCPU pVCpu, void *pvUser)
{
    RT_NOREF(pVM);
    VMIR3EMULATE *pReq = (VMIR3EMULATE *)pvUser;
    if (pVCpu->idCpu == pReq->idCpu)
        pReq->rc = vmiR3Emulate(pVCpu, pReq->uAction, pReq->pbData, pReq->cbData, pReq->prcEmulated);
    return VINF_SUCCESS;
}

#endif /* VBOX_VMM_TARGET_X86 */

/**
 * Accepts an event reply on the parked vCPU's EMT; an emulation reply runs the
 * instruction here, so that its failure leaves the vCPU parked.
 *
 * @returns VBox status code for the reply.
 * @param   pVM     The cross context VM structure.
 * @param   pThis   The VBoxVMI instance.
 * @param   pReq    The reply, validated but for the event it applies to.
 * @param   pbData  The bytes following it.
 */
static DECLCALLBACK(int) vmiR3ReplyOnEmt(PVM pVM, PVMIR3 pThis, VMIEVENTREPLY const *pReq, uint8_t const *pbData)
{
    PVMCPU pVCpu = VMMGetCpu(pVM);
    PVMIR3CPU const pCpu = &pThis->paCpus[pVCpu->idCpu];
    if (!pCpu->fParked)
        return VERR_INVALID_STATE;
    switch (pReq->uAction)
    {
        case VMI_EVENT_REPLY_CONTINUE:
            if ((pReq->fFlags & VMI_EVENT_REPLY_F_REINJECT) && pCpu->uKind != VMI_INTERCEPT_BREAKPOINT)
                return VERR_INVALID_PARAMETER;
            break;
        case VMI_EVENT_REPLY_EMULATE:
        case VMI_EVENT_REPLY_EMULATE_READ_DATA:
            if (pCpu->uKind != VMI_INTERCEPT_PAGE_ACCESS)
                return VERR_INVALID_PARAMETER;
            break;
        default:
            if (pCpu->uKind != VMI_INTERCEPT_BREAKPOINT)
                return VERR_INVALID_PARAMETER;
            break;
    }
    if (pReq->uAction != VMI_EVENT_REPLY_CONTINUE)
    {
#ifdef VBOX_VMM_TARGET_X86
        VMIR3EMULATE Emulate = { pVCpu->idCpu, pReq->uAction, pbData, pReq->cbData, &pCpu->rcEmulated, VINF_SUCCESS };
        int rc = VMMR3EmtRendezvous(pVM, VMMEMTRENDEZVOUS_FLAGS_TYPE_ONE_BY_ONE, vmiR3EmulateRendezvous, &Emulate);
        if (RT_SUCCESS(rc))
            rc = Emulate.rc;
        if (RT_FAILURE(rc))
            return rc;
        pCpu->fEmulated = true;
#else
        RT_NOREF(pbData);
        return VERR_NOT_SUPPORTED;
#endif
    }
    pCpu->fReinject = RT_BOOL(pReq->fFlags & VMI_EVENT_REPLY_F_REINJECT);
    pCpu->fParked = false;
    return VINF_SUCCESS;
}


/**
 * Wakes the server thread up, for an event or termination.
 */
static void vmiR3Wakeup(PVMIR3 pThis)
{
    char ch = 0;
    ssize_t cbWritten;
    do
        cbWritten = write(pThis->afdWakeup[1], &ch, 1);
    while (cbWritten < 0 && errno == EINTR);
    /* EAGAIN: the pipe is full, so a wakeup is pending already. */
    if (cbWritten < 0 && errno != EAGAIN)
        LogRel(("VMI: Cannot wake the server thread up: errno=%d\n", errno));
}


/**
 * Reports a VBoxVMI event and parks the calling EMT until the client replies.
 *
 * Called by EM's debug loop on the event's EMT.  The parked EMT still serves
 * its own requests and VM-wide rendezvous (pause and resume included, the vCPU
 * staying parked), until the reply, the end of the session or VM termination.
 *
 * @returns Strict VBox status code for EM: VINF_EM_RESCHEDULE or
 *          VINF_EM_SUSPEND to continue, VINF_EM_DBG_STEPPED after a CR or MSR
 *          write or an emulation reply executed while single stepping, or the
 *          status ending the park (VINF_EM_OFF, VINF_EM_RESET,
 *          VINF_EM_TERMINATE, failure).
 * @param   pVM     The cross context VM structure.
 * @param   pVCpu   The cross context virtual CPU structure of the calling EMT.
 * @param   uKind   The event kind, VMI_INTERCEPT_XXX.  For CR and MSR writes the
 *                  VM-exit handler left the register and new value in
 *                  HMCPU::Vmi; the write executes here after the reply.  For
 *                  page accesses PGMPhysVmiCheckAccess left the access in
 *                  PGMCPU::Vmi; the instruction re-executes after the reply,
 *                  unless an emulation reply ran it.
 */
VMMR3_INT_DECL(VBOXSTRICTRC) VMIR3DebugEvent(PVM pVM, PVMCPU pVCpu, uint32_t uKind)
{
#ifdef VBOX_VMM_TARGET_X86
    uint8_t const fHitAccess = pVCpu->pgm.s.Vmi.fHitAccess;
    pVCpu->pgm.s.Vmi.fHitAccess = 0;
#endif
    PVMIR3 pThis = (PVMIR3)pVM->pUVM->vm.s.pvVMI;
    if (!pThis || !pThis->fHandshaked)
        return VINF_EM_RESCHEDULE; /* A pending write or access re-executes, its intercept is cleared already. */

    VMIEVENT Event;
    RT_ZERO(Event);
    Event.idCpu = pVCpu->idCpu;
    Event.uKind = uKind;
    bool const fWrite = uKind == VMI_INTERCEPT_CR_WRITE || uKind == VMI_INTERCEPT_MSR_WRITE;
    int rc = VINF_SUCCESS;
    if (fWrite)
    {
#ifdef VBOX_VMM_TARGET_X86
        Event.uReg = pVCpu->hm.s.Vmi.uHitReg;
        Event.uNew = pVCpu->hm.s.Vmi.uHitNew;
        if (uKind == VMI_INTERCEPT_MSR_WRITE)
            Event.uOld = vmiR3QueryMsr(pVCpu, Event.uReg);
        else
            Event.uOld = Event.uReg == 0 ? CPUMGetGuestCR0(pVCpu)
                       : Event.uReg == 3 ? CPUMGetGuestCR3(pVCpu) : CPUMGetGuestCR4(pVCpu);
#endif
    }
#ifdef VBOX_VMM_TARGET_X86
    else if (uKind == VMI_INTERCEPT_PAGE_ACCESS)
    {
        Event.GCPhys = pVCpu->pgm.s.Vmi.GCPhysHit;
        Event.uReg   = fHitAccess;
        Event.GCPtr  = pVCpu->pgm.s.Vmi.GCPtrHit;
    }
#endif
    else
    {
        /* An unmapped RIP (a step onto a page not faulted in yet) is reported, not fatal to EM. */
        rc = PGMPhysGCPtr2GCPhys(pVCpu, CPUMGetGuestFlatPC(pVCpu), &Event.GCPhys);
        if (RT_FAILURE(rc))
            Event.GCPhys = NIL_RTGCPHYS;
    }
    if (uKind == VMI_INTERCEPT_BREAKPOINT)
    {
        DISSTATE Dis;
        unsigned cbInstr;
        rc = EMInterpretDisasCurrent(pVCpu, &Dis, &cbInstr);
        /* shortcut: assumes the one-byte 0xcc if the int3 can't be decoded again (it just executed). */
        Event.cbInstr = RT_SUCCESS(rc) ? cbInstr : 1;
    }

    uint32_t const idCpu = pVCpu->idCpu;
    pThis->paCpus[idCpu].uKind = uKind;
    pThis->paCpus[idCpu].cbInstr = Event.cbInstr;
    pThis->paCpus[idCpu].fReinject = false;
    pThis->paCpus[idCpu].fEmulated = false;
    pThis->paCpus[idCpu].fParked = true;
    RTSemFastMutexRequest(pThis->hMtxEvents);
    Assert(pThis->cEvents < pVM->cCpus);
    pThis->paEvents[pThis->iEventWrite] = Event;
    pThis->iEventWrite = (pThis->iEventWrite + 1) % pVM->cCpus;
    pThis->cEvents++;
    RTSemFastMutexRelease(pThis->hMtxEvents);
    vmiR3Wakeup(pThis);

    /* A write waits for the resume too: it must not execute while the VM is paused. */
    while (   pThis->paCpus[idCpu].fParked
           || (fWrite && VMR3GetState(pVM) >= VMSTATE_SUSPENDED && VMR3GetState(pVM) <= VMSTATE_SUSPENDED_EXT_LS))
    {
        if (VM_FF_IS_SET(pVM, VM_FF_EMT_RENDEZVOUS))
            rc = VMMR3EmtRendezvousFF(pVM, pVCpu);
        else if (VM_FF_IS_SET(pVM, VM_FF_REQUEST) || VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_REQUEST))
        {
            rc = VMR3ReqProcessU(pVM->pUVM, VMCPUID_ANY, false /*fPriorityOnly*/);
            if (rc == VINF_SUCCESS)
                rc = VMR3ReqProcessU(pVM->pUVM, idCpu, false /*fPriorityOnly*/);
        }
        else if (VMR3GetState(pVM) >= VMSTATE_POWERING_OFF)
            rc = VINF_EM_OFF;
        else
        {
            rc = VMR3WaitU(pVCpu->pUVCpu);
            if (RT_SUCCESS(rc))
                continue;
        }
        if (   rc == VINF_EM_RESET
            || rc == VINF_EM_OFF
            || rc == VINF_EM_TERMINATE
            || RT_FAILURE(rc))
        {
            pThis->paCpus[idCpu].fParked = false;
            return rc;
        }
    }

    if (pThis->paCpus[idCpu].fReinject)
    {
        rc = TRPMAssertTrap(pVCpu, X86_XCPT_BP, TRPM_SOFTWARE_INT);
        if (RT_FAILURE(rc))
            return rc;
        TRPMSetInstrLength(pVCpu, (uint8_t)pThis->paCpus[idCpu].cbInstr);
    }

#ifdef VBOX_VMM_TARGET_X86
    if (pThis->paCpus[idCpu].fEmulated)
    {
        /* An emulation reply ran the instruction on this EMT already (vmiR3ReplyOnEmt), also while paused. */
        VBOXSTRICTRC const rcStrict = pThis->paCpus[idCpu].rcEmulated;
        if (   rcStrict >= VINF_EM_FIRST
            && rcStrict <= VINF_EM_LAST
            && rcStrict != VINF_EM_RESCHEDULE /* == VINF_IEM_RAISED_XCPT, the guest got its exception */)
            return rcStrict;
        if (pVCpu->dbgf.s.fVmiSingleStep)
            return VINF_EM_DBG_STEPPED;
    }

    if (fWrite)
    {
        /* Execute the write here: back in VT-x it would exit again.  The client may have changed its source register. */
        IEMTlbInvalidateAll(pVCpu);
        VBOXSTRICTRC rcStrict = IEMExecOne(pVCpu);
        if (rcStrict == VERR_IEM_INSTR_NOT_IMPLEMENTED || rcStrict == VERR_IEM_ASPECT_NOT_IMPLEMENTED)
        {
            /* Let VT-x execute this write once if IEM cannot handle it. */
            pVCpu->hm.s.Vmi.uSkipRip  = pVCpu->cpum.GstCtx.rip;
            pVCpu->hm.s.Vmi.uSkipKind = Event.uKind;
            pVCpu->hm.s.Vmi.uSkipReg  = Event.uReg;
            pVCpu->hm.s.Vmi.fSkipHit  = true;
            return VINF_EM_RESCHEDULE;
        }
        if (   RT_FAILURE(rcStrict)
            || (   rcStrict >= VINF_EM_FIRST
                && rcStrict <= VINF_EM_LAST
                && rcStrict != VINF_EM_RESCHEDULE /* == VINF_IEM_RAISED_XCPT, the guest got its #GP */))
            return rcStrict;
        /* Other informational statuses (PGM syncs) come with force flags HM handles. */
        return pVCpu->dbgf.s.fVmiSingleStep ? VINF_EM_DBG_STEPPED : VINF_EM_RESCHEDULE;
    }

#endif /* VBOX_VMM_TARGET_X86 */
    /* Suspended meanwhile, live save or snapshot included: the vCPU continues on resume. */
    switch (VMR3GetState(pVM))
    {
        case VMSTATE_SUSPENDED:
        case VMSTATE_SUSPENDED_LS:
        case VMSTATE_SUSPENDED_EXT_LS:
            return VINF_EM_SUSPEND;
        default:
            return VINF_EM_RESCHEDULE;
    }
}


/** Sets or clears one intercept; CPU-local changes run on its EMT. */
static bool vmiR3HandleIntercept(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    VMIINTERCEPT Req;
    int rc = cbBody == sizeof(Req) ? VINF_SUCCESS : VERR_INVALID_PARAMETER;
    if (RT_SUCCESS(rc))
    {
        memcpy(&Req, (uint8_t const *)pHdr + sizeof(*pHdr), sizeof(Req));
        if (Req.u32Reserved)
            rc = VERR_INVALID_PARAMETER;
        else if (Req.idCpu >= pThis->pUVM->cCpus)
            rc = VERR_INVALID_CPU_ID;
        else if (   (   Req.uKind == VMI_INTERCEPT_BREAKPOINT
                     || Req.uKind == VMI_INTERCEPT_SINGLESTEP
                     || Req.uKind == VMI_INTERCEPT_PAGE_ACCESS)
                 && Req.uReg)
            rc = VERR_INVALID_PARAMETER;
        else if (   Req.uKind < VMI_INTERCEPT_BREAKPOINT
                 || Req.uKind > VMI_INTERCEPT_PAGE_ACCESS
                 || (Req.uKind == VMI_INTERCEPT_CR_WRITE && Req.uReg != 0 && Req.uReg != 3 && Req.uReg != 4))
            rc = VERR_NOT_SUPPORTED;
#ifdef VBOX_VMM_TARGET_X86
        else
            rc = VMR3ReqCallWaitU(pThis->pUVM, Req.idCpu, (PFNRT)vmiR3InterceptOnEmt, 4,
                                  pThis->pUVM->pVM, Req.uKind, Req.uReg, pHdr->uType == VMI_MSG_INTERCEPT_SET);
#else
        else
            rc = VERR_NOT_SUPPORTED;
#endif
    }
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, rc, NULL, 0));
}


/**
 * Sets (in an EMT rendezvous) or gets the page access restriction of a page.
 */
static bool vmiR3HandlePageAccess(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    bool const    fSet = pHdr->uType == VMI_MSG_PAGE_ACCESS_SET;
    uint32_t      fAllowed = VMI_PAGE_ACCESS_RWX;
    VMIPAGEACCESS Req;
    int rc = cbBody == sizeof(Req) ? VINF_SUCCESS : VERR_INVALID_PARAMETER;
    if (RT_SUCCESS(rc))
    {
        memcpy(&Req, (uint8_t const *)pHdr + sizeof(*pHdr), sizeof(Req));
        if (   Req.u32Reserved
            || (Req.fAccess & ~VMI_PAGE_ACCESS_RWX)
            || (fSet ? (Req.fAccess & (VMI_PAGE_ACCESS_R | VMI_PAGE_ACCESS_W)) == VMI_PAGE_ACCESS_W : Req.fAccess != 0))
            rc = VERR_INVALID_PARAMETER;
#ifdef VBOX_VMM_TARGET_X86
        else if (!HMR3IsVmxEnabled(pThis->pUVM) || !HMR3IsNestedPagingActive(pThis->pUVM))
            rc = VERR_NOT_SUPPORTED; /* The restrictions live in the EPT entries. */
        else if (   fSet
                 && Req.fAccess == VMI_PAGE_ACCESS_X
                 && !(pThis->pUVM->pVM->hm.s.ForR3.vmx.Msrs.u64EptVpidCaps & VMX_BF_EPT_VPID_CAP_EXEC_ONLY_MASK))
            rc = VERR_NOT_SUPPORTED;
        else if (fSet)
        {
            VMIR3PAGEACCESSSET Set = { Req.GCPhys, Req.fAccess, VINF_SUCCESS };
            rc = VMMR3EmtRendezvous(pThis->pUVM->pVM, VMMEMTRENDEZVOUS_FLAGS_TYPE_ONCE, vmiR3PageAccessSetRendezvous, &Set);
            if (RT_SUCCESS(rc))
                rc = Set.rc;
        }
        else
        {
            PVM const pVM = pThis->pUVM->pVM;
            PGM_LOCK_VOID(pVM);
            PPGMPAGE pPage;
            rc = vmiR3PageAccessLookup(pVM, Req.GCPhys, &pPage);
            if (RT_SUCCESS(rc))
                fAllowed = VMI_PAGE_ACCESS_RWX & ~PGM_PAGE_GET_VMI_DENIED(pPage);
            PGM_UNLOCK(pVM);
        }
#else
        else
            rc = VERR_NOT_SUPPORTED;
#endif
    }
    if (RT_SUCCESS(rc))
        rc = VINF_SUCCESS;
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, rc,
                                &fAllowed, RT_SUCCESS(rc) && !fSet ? sizeof(fAllowed) : 0));
}


/** Validates the reply before handing it to its parked EMT. */
static bool vmiR3HandleEventReply(PVMIR3 pThis, VMIMSGHDR const *pHdr, size_t cbBody)
{
    VMIEVENTREPLY Req;
    int rc = cbBody >= sizeof(Req) ? VINF_SUCCESS : VERR_INVALID_PARAMETER;
    if (RT_SUCCESS(rc))
    {
        uint8_t const *pbData = (uint8_t const *)pHdr + sizeof(*pHdr) + sizeof(Req);
        memcpy(&Req, (uint8_t const *)pHdr + sizeof(*pHdr), sizeof(Req));
        if (   (Req.fFlags & ~VMI_EVENT_REPLY_F_REINJECT)
            || Req.cbData != cbBody - sizeof(Req)
            || Req.uAction > VMI_EVENT_REPLY_EMULATE_INSN
            || ((Req.fFlags & VMI_EVENT_REPLY_F_REINJECT) && Req.uAction != VMI_EVENT_REPLY_CONTINUE)
            || (  Req.uAction <= VMI_EVENT_REPLY_EMULATE           ? Req.cbData != 0
                : Req.uAction == VMI_EVENT_REPLY_EMULATE_READ_DATA ? Req.cbData - 1 >= VMI_EVENT_REPLY_READ_DATA_MAX
                :                                                    Req.cbData != VMI_EVENT_REPLY_INSN_SIZE))
            rc = VERR_INVALID_PARAMETER;
        else if (Req.idCpu >= pThis->pUVM->cCpus)
            rc = VERR_INVALID_CPU_ID;
        else
            rc = VMR3ReqCallWaitU(pThis->pUVM, Req.idCpu, (PFNRT)vmiR3ReplyOnEmt, 4,
                                  pThis->pUVM->pVM, pThis, &Req, pbData);
    }
    if (RT_SUCCESS(rc))
        rc = VINF_SUCCESS;
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, pHdr->idReq, pHdr->uType, VMI_MSG_F_REPLY, rc, NULL, 0));
}


/**
 * Receives and handles one message from the client.
 *
 * @returns false if the client must be disconnected.
 */
static bool vmiR3HandleMsg(PVMIR3 pThis)
{
    /* MSG_TRUNC makes recv return the real size of an oversized message. */
    ssize_t cbMsg;
    do
        cbMsg = recv(pThis->fdClient, pThis->abMsg, sizeof(pThis->abMsg), MSG_TRUNC);
    while (cbMsg < 0 && errno == EINTR);
    if (cbMsg <= 0)
        return false; /* EOF or error */

    VMIMSGHDR Hdr;
    if (   (size_t)cbMsg > sizeof(pThis->abMsg)
        || (size_t)cbMsg < sizeof(Hdr))
    {
        LogRel(("VMI: Protocol error: message size %zd\n", cbMsg));
        return false;
    }
    memcpy(&Hdr, pThis->abMsg, sizeof(Hdr));
    if (   Hdr.cbMsg  != (size_t)cbMsg
        || Hdr.fFlags != 0
        || Hdr.rc     != 0)
    {
        LogRel(("VMI: Protocol error: bad request header (cbMsg=%#x/%#zx fFlags=%#x rc=%d)\n",
                Hdr.cbMsg, cbMsg, Hdr.fFlags, Hdr.rc));
        return false;
    }

    size_t const cbBody = (size_t)cbMsg - sizeof(Hdr);
    if (Hdr.uType == VMI_MSG_HELLO)
        return vmiR3HandleHello(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
    if (!pThis->fHandshaked)
    {
        vmiR3Send(pThis->fdClient, Hdr.idReq, Hdr.uType, VMI_MSG_F_REPLY, VERR_WRONG_ORDER, NULL, 0);
        return false;
    }
    switch (Hdr.uType)
    {
        case VMI_MSG_INTERCEPT_SET:
        case VMI_MSG_INTERCEPT_CLEAR:
            return vmiR3HandleIntercept(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_EVENT_REPLY:
            return vmiR3HandleEventReply(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_PAGE_ACCESS_SET:
        case VMI_MSG_PAGE_ACCESS_GET:
            return vmiR3HandlePageAccess(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_PHYS_READ:
            return vmiR3HandlePhysRead(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_PHYS_WRITE:
            return vmiR3HandlePhysWrite(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_RAM_RANGES:
            return vmiR3HandleRamRanges(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_PAUSE:
        case VMI_MSG_RESUME:
            return vmiR3HandlePauseResume(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_VCPU_COUNT:
            return vmiR3HandleVCpuCount(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
#ifdef VBOX_VMM_TARGET_X86
        case VMI_MSG_REGS_READ:
        case VMI_MSG_REGS_WRITE:
            return vmiR3HandleRegs(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
#endif
        default:
            break;
    }
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, Hdr.idReq, Hdr.uType, VMI_MSG_F_REPLY, VERR_NOT_SUPPORTED, NULL, 0));
}


/** Only the server thread writes the socket, in event occurrence order. */
static bool vmiR3SendEvents(PVMIR3 pThis)
{
    for (;;)
    {
        VMIEVENT Event;
        RTSemFastMutexRequest(pThis->hMtxEvents);
        if (!pThis->cEvents)
        {
            RTSemFastMutexRelease(pThis->hMtxEvents);
            return true;
        }
        Event = pThis->paEvents[pThis->iEventRead];
        pThis->iEventRead = (pThis->iEventRead + 1) % pThis->pUVM->cCpus;
        pThis->cEvents--;
        RTSemFastMutexRelease(pThis->hMtxEvents);
        if (RT_FAILURE(vmiR3Send(pThis->fdClient, 0, VMI_MSG_EVENT, 0, VINF_SUCCESS, &Event, sizeof(Event))))
            return false;
    }
}


/**
 * @callback_method_impl{FNRTTHREAD, The VBoxVMI server thread.}
 */
static DECLCALLBACK(int) vmiR3Thread(RTTHREAD hThreadSelf, void *pvUser)
{
    RT_NOREF(hThreadSelf);
    PVMIR3 pThis = (PVMIR3)pvUser;

    for (;;)
    {
        int cMsTimeout = -1;
        bool fAccept = true;
        if (pThis->msAcceptResume)
        {
            uint64_t const msNow = RTTimeMilliTS();
            if (msNow < pThis->msAcceptResume)
            {
                fAccept    = false;
                cMsTimeout = (int)(pThis->msAcceptResume - msNow);
            }
            else
                pThis->msAcceptResume = 0;
        }

        struct pollfd aFds[3];
        aFds[0].fd = pThis->afdWakeup[0];
        aFds[1].fd = fAccept ? pThis->fdListen : -1; /* poll ignores negative descriptors */
        aFds[2].fd = pThis->fdClient;
        for (unsigned i = 0; i < RT_ELEMENTS(aFds); i++)
        {
            aFds[i].events  = POLLIN;
            aFds[i].revents = 0;
        }

        int cReady = poll(aFds, RT_ELEMENTS(aFds), cMsTimeout);
        if (cReady < 0)
        {
            if (errno == EINTR)
                continue;
            LogRel(("VMI: poll failed: errno=%d, server stopped\n", errno));
            break;
        }

        if (aFds[0].revents)
        {
            char abWake[64];
            while (read(pThis->afdWakeup[0], abWake, sizeof(abWake)) > 0)
                { /* Drain the nonblocking pipe. */ }
            if (ASMAtomicReadBool(&pThis->fTerminating))
                break;
            if (pThis->fdClient >= 0 && !vmiR3SendEvents(pThis))
                vmiR3Disconnect(pThis);
        }
        if (   aFds[2].revents
            && pThis->fdClient >= 0 /* not disconnected by a failed event send */
            && !vmiR3HandleMsg(pThis))
            vmiR3Disconnect(pThis);
        if (aFds[1].revents)
            vmiR3Accept(pThis);
    }

    if (pThis->fdClient >= 0)
        vmiR3Disconnect(pThis);
    return VINF_SUCCESS;
}


/**
 * Checks that the socket's directory is private to us: owned by our euid, and
 * not writable by others unless sticky.  Otherwise another user could replace
 * the socket file under the client's feet.
 */
static int vmiR3CheckDir(PUVM pUVM, const char *pszDir)
{
    struct stat St;
    if (stat(pszDir, &St) != 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS,
                            "VMI: Cannot stat the socket directory \"%s\" (errno=%d)", pszDir, errno);
    if (!S_ISDIR(St.st_mode))
        return VMR3SetError(pUVM, VERR_NOT_A_DIRECTORY, RT_SRC_POS, "VMI: \"%s\" is not a directory", pszDir);
    if (St.st_uid != geteuid())
        return VMR3SetError(pUVM, VERR_ACCESS_DENIED, RT_SRC_POS,
                            "VMI: The socket directory \"%s\" is owned by uid %u, not by the user running the VM",
                            pszDir, St.st_uid);
    if ((St.st_mode & (S_IWGRP | S_IWOTH)) && !(St.st_mode & S_ISVTX))
        return VMR3SetError(pUVM, VERR_ACCESS_DENIED, RT_SRC_POS,
                            "VMI: The socket directory \"%s\" is writable by group or others (mode %o)",
                            pszDir, St.st_mode & 07777);
    return VINF_SUCCESS;
}


/**
 * Handles a socket file already at pThis->pszPath: removed if stale (nobody
 * listening, e.g. left by a crashed VM process), an error otherwise.
 */
static int vmiR3RemoveStaleSocket(PVMIR3 pThis, struct sockaddr_un const *pAddr)
{
    PUVM pUVM = pThis->pUVM;
    struct stat St;
    if (lstat(pThis->pszPath, &St) != 0)
        return VINF_SUCCESS;
    if (!S_ISSOCK(St.st_mode))
        return VMR3SetError(pUVM, VERR_ALREADY_EXISTS, RT_SRC_POS, "VMI: \"%s\" exists and is not a socket", pThis->pszPath);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS, "VMI: socket failed (errno=%d)", errno);
    int rcConnect;
    do
        rcConnect = connect(fd, (struct sockaddr const *)pAddr, sizeof(*pAddr));
    while (rcConnect != 0 && errno == EINTR);
    int const iErr = errno;
    close(fd);
    if (rcConnect == 0)
        return VMR3SetError(pUVM, VERR_RESOURCE_IN_USE, RT_SRC_POS,
                            "VMI: The socket \"%s\" is in use by another process (another VM with the same VMI/SocketPath?)",
                            pThis->pszPath);
    if (iErr != ECONNREFUSED)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(iErr), RT_SRC_POS,
                            "VMI: Cannot tell whether the socket \"%s\" is in use (errno=%d)", pThis->pszPath, iErr);
    LogRel(("VMI: Removing the stale socket %s\n", pThis->pszPath));
    if (unlink(pThis->pszPath) != 0 && errno != ENOENT)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS,
                            "VMI: Failed to remove the stale socket \"%s\" (errno=%d)", pThis->pszPath, errno);
    return VINF_SUCCESS;
}


/**
 * Creates the listening socket at pThis->pszPath, mode 0600.
 *
 * @returns VBox status code, with the VM error set on failure.
 */
static int vmiR3Listen(PVMIR3 pThis)
{
    PUVM pUVM = pThis->pUVM;
    struct sockaddr_un Addr;
    RT_ZERO(Addr);
    Addr.sun_family = AF_UNIX;
    int rc = RTStrCopy(Addr.sun_path, sizeof(Addr.sun_path), pThis->pszPath);
    if (RT_FAILURE(rc))
        return VMR3SetError(pUVM, VERR_FILENAME_TOO_LONG, RT_SRC_POS,
                            "VMI: The socket path \"%s\" is longer than %zu bytes", pThis->pszPath, sizeof(Addr.sun_path) - 1);
    if (!RTPathStartsWithRoot(pThis->pszPath))
        return VMR3SetError(pUVM, VERR_PATH_IS_RELATIVE, RT_SRC_POS, "VMI: The socket path \"%s\" is not absolute",
                            pThis->pszPath);

    /* The default path's directory (<VBox user home>/vmi) may not exist yet. */
    char szDir[RTPATH_MAX];
    RTStrCopy(szDir, sizeof(szDir), pThis->pszPath);
    RTPathStripFilename(szDir);
    rc = RTDirCreateFullPath(szDir, 0700);
    if (RT_FAILURE(rc))
        return VMR3SetError(pUVM, rc, RT_SRC_POS, "VMI: Failed to create the directory \"%s\"", szDir);
    rc = vmiR3CheckDir(pUVM, szDir);
    if (RT_SUCCESS(rc))
        rc = vmiR3RemoveStaleSocket(pThis, &Addr);
    if (RT_FAILURE(rc))
        return rc;

    pThis->fdListen = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (pThis->fdListen < 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS, "VMI: socket failed (errno=%d)", errno);
    /* Linux creates the socket file with the socket inode's mode (minus the umask),
       so the file is 0600 (or stricter) from the start, no chmod by path. */
    if (fchmod(pThis->fdListen, 0600) != 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS, "VMI: fchmod failed (errno=%d)", errno);
    if (bind(pThis->fdListen, (struct sockaddr *)&Addr, sizeof(Addr)) != 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS,
                            "VMI: Failed to bind \"%s\" (errno=%d)", pThis->pszPath, errno);
    pThis->fBound = true;

    struct stat St;
    if (lstat(pThis->pszPath, &St) != 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS,
                            "VMI: Cannot stat \"%s\" after bind (errno=%d)", pThis->pszPath, errno);
    pThis->devSock = St.st_dev;
    pThis->inoSock = St.st_ino;
    if (St.st_mode & 077)
        return VMR3SetError(pUVM, VERR_ACCESS_DENIED, RT_SRC_POS,
                            "VMI: The socket \"%s\" was created with mode %o instead of 0600", pThis->pszPath, St.st_mode & 0777);

    if (listen(pThis->fdListen, 1) != 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS,
                            "VMI: Failed to listen on \"%s\" (errno=%d)", pThis->pszPath, errno);
    return VINF_SUCCESS;
}


/**
 * Starts the VBoxVMI server if the VM config enables it.
 *
 * @returns VBox status code, with the VM error set on failure.
 * @param   pUVM        The user mode VM handle.
 * @param   ppvVmi      Where to store the server instance, NULL if disabled.
 */
VMMR3_INT_DECL(int) VMIR3Create(PUVM pUVM, void **ppvVmi)
{
    *ppvVmi = NULL;

    PCFGMNODE pCfg = CFGMR3GetChild(CFGMR3GetRootU(pUVM), "VMI");
    int rc = CFGMR3ValidateConfig(pCfg, "/VMI/", "Enabled|SocketPath", "", "VMI", 0 /*iInstance*/);
    if (RT_FAILURE(rc))
        return rc;
    bool fEnabled;
    rc = CFGMR3QueryBoolDef(pCfg, "Enabled", &fEnabled, false);
    if (RT_FAILURE(rc))
        return VM_SET_ERROR_U(pUVM, rc, "Configuration error: Failed querying \"VMI/Enabled\"");
    if (!fEnabled)
        return VINF_SUCCESS;

    PVMIR3 pThis = (PVMIR3)RTMemAllocZ(sizeof(*pThis));
    if (!pThis)
        return VERR_NO_MEMORY;
    pThis->pUVM         = pUVM;
    pThis->hThread      = NIL_RTTHREAD;
    pThis->fdListen     = -1;
    pThis->fdClient     = -1;
    pThis->hMtxClient   = NIL_RTSEMFASTMUTEX;
    pThis->hMtxEvents   = NIL_RTSEMFASTMUTEX;
    pThis->afdWakeup[0] = -1;
    pThis->afdWakeup[1] = -1;

    /* Main sets the default, <VBox user home>/vmi/<VM UUID>.sock. */
    rc = CFGMR3QueryStringAlloc(pCfg, "SocketPath", &pThis->pszPath);
    if (RT_FAILURE(rc))
        rc = VM_SET_ERROR_U(pUVM, rc, "Configuration error: Failed querying \"VMI/SocketPath\"");
    if (RT_SUCCESS(rc))
        rc = RTSemFastMutexCreate(&pThis->hMtxClient);
    if (RT_SUCCESS(rc))
        rc = RTSemFastMutexCreate(&pThis->hMtxEvents);
    if (RT_SUCCESS(rc))
    {
        pThis->paEvents = (VMIEVENT *)RTMemAllocZ(pUVM->cCpus * sizeof(*pThis->paEvents));
        pThis->paCpus = (PVMIR3CPU)RTMemAllocZ(pUVM->cCpus * sizeof(*pThis->paCpus));
        if (!pThis->paEvents || !pThis->paCpus)
            rc = VERR_NO_MEMORY;
    }
    if (RT_SUCCESS(rc))
        rc = vmiR3Listen(pThis);
    if (RT_SUCCESS(rc) && pipe2(pThis->afdWakeup, O_CLOEXEC | O_NONBLOCK) != 0)
        rc = VM_SET_ERROR_U(pUVM, RTErrConvertFromErrno(errno), "VMI: pipe2 failed");
    if (RT_SUCCESS(rc))
    {
        rc = VMR3AtStateRegister(pUVM, vmiR3AtState, pThis);
        pThis->fAtStateRegistered = RT_SUCCESS(rc);
    }
    if (RT_SUCCESS(rc))
    {
        /* EMTs may raise an event as soon as the server accepts a client. */
        *ppvVmi = pThis;
        rc = RTThreadCreate(&pThis->hThread, vmiR3Thread, pThis, 0 /*cbStack*/, RTTHREADTYPE_IO,
                            RTTHREADFLAGS_WAITABLE, "VMI");
        if (RT_SUCCESS(rc))
        {
            LogRel(("VMI: Listening on %s\n", pThis->pszPath));
            return VINF_SUCCESS;
        }
        pThis->hThread = NIL_RTTHREAD;
        *ppvVmi = NULL;
        rc = VM_SET_ERROR_U(pUVM, rc, "VMI: Failed to create the server thread");
    }

    VMIR3Terminate(pUVM, pThis);
    return rc;
}


/**
 * Stops the VBoxVMI server, disconnecting the client and removing the socket.
 *
 * Called by VMR3Destroy before the EMTs stop serving requests, so the server
 * thread can still finish a request it routed to an EMT.  Never waits for the
 * thread indefinitely.
 *
 * @param   pUVM        The user mode VM handle.
 * @param   pvVmi       The server instance from VMIR3Create, NULL is fine.
 */
VMMR3_INT_DECL(void) VMIR3Terminate(PUVM pUVM, void *pvVmi)
{
    PVMIR3 pThis = (PVMIR3)pvVmi;
    if (!pThis)
        return;


    if (pThis->hThread != NIL_RTTHREAD)
    {
        ASMAtomicWriteBool(&pThis->fTerminating, true);
        vmiR3Wakeup(pThis);

        /* Unblock a send to or recv from the client. */
        RTSemFastMutexRequest(pThis->hMtxClient);
        if (pThis->fdClient >= 0)
            shutdown(pThis->fdClient, SHUT_RDWR);
        RTSemFastMutexRelease(pThis->hMtxClient);

        int rc = RTThreadWait(pThis->hThread, VMI_TERM_TIMEOUT_MS, NULL);
        if (RT_FAILURE(rc))
        {
            /* The thread still uses the instance: leak it rather than hang the VM teardown. */
            LogRel(("VMI: The server thread did not stop (%Rrc), abandoning it\n", rc));
            return;
        }
    }
    if (pThis->fAtStateRegistered)
    {
        int rc = VMR3AtStateDeregister(pUVM, vmiR3AtState, pThis);
        AssertRC(rc);
        pThis->fAtStateRegistered = false;
    }

    if (pThis->fdListen >= 0)
        close(pThis->fdListen);
    if (pThis->fBound)
    {
        /* Only if the file is still the one we bound. */
        struct stat St;
        if (   lstat(pThis->pszPath, &St) == 0
            && S_ISSOCK(St.st_mode)
            && St.st_dev == pThis->devSock
            && St.st_ino == pThis->inoSock)
            unlink(pThis->pszPath);
    }
    if (pThis->afdWakeup[0] >= 0)
        close(pThis->afdWakeup[0]);
    if (pThis->afdWakeup[1] >= 0)
        close(pThis->afdWakeup[1]);
    RTSemFastMutexDestroy(pThis->hMtxClient);
    if (pThis->hMtxEvents != NIL_RTSEMFASTMUTEX)
        RTSemFastMutexDestroy(pThis->hMtxEvents);
    RTMemFree(pThis->paEvents);
    RTMemFree(pThis->paCpus);
    MMR3HeapFree(pThis->pszPath);
    RTMemFree(pThis);
}
