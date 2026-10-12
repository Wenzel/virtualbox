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
#include <VBox/vmm/vmi.h>
#include <VBox/vmm/cfgm.h>
#include <VBox/vmm/mm.h>
#include <VBox/vmm/pgm.h>
#include <VBox/vmm/vm.h>
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
    /** Pipe for waking up the server thread on termination: [0] read, [1] write. */
    int             afdWakeup[2];
    /** Set when the client completed the handshake. */
    bool            fHandshaked;
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
 * Drops the connected client.
 *
 * This is where a client session ends, whatever the reason, so this is where
 * the server will undo what the client did to the VM (resume it if
 * vmiR3IsPausedByClient, intercepts).  For now the VM stays as the client left
 * it.
 */
static void vmiR3Disconnect(PVMIR3 pThis)
{
    LogRel(("VMI: Client disconnected%s\n", vmiR3IsPausedByClient(pThis) ? ", leaving the VM paused by the client" : ""));
    RTSemFastMutexRequest(pThis->hMtxClient);
    close(pThis->fdClient);
    pThis->fdClient = -1;
    RTSemFastMutexRelease(pThis->hMtxClient);
    pThis->fHandshaked     = false;
    pThis->fPausedByClient = false;
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
        pThis->fHandshaked = false;
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
    pThis->fHandshaked = RT_SUCCESS(rc) && RT_SUCCESS(rcReply);
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
        case VMI_MSG_PHYS_READ:
            return vmiR3HandlePhysRead(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_RAM_RANGES:
            return vmiR3HandleRamRanges(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_PAUSE:
        case VMI_MSG_RESUME:
            return vmiR3HandlePauseResume(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        case VMI_MSG_VCPU_COUNT:
            return vmiR3HandleVCpuCount(pThis, (VMIMSGHDR const *)pThis->abMsg, cbBody);
        default:
            break;
    }
    return RT_SUCCESS(vmiR3Send(pThis->fdClient, Hdr.idReq, Hdr.uType, VMI_MSG_F_REPLY, VERR_NOT_SUPPORTED, NULL, 0));
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
            break; /* VMIR3Terminate */
        if (   aFds[2].revents
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
    pThis->afdWakeup[0] = -1;
    pThis->afdWakeup[1] = -1;

    /* Main sets the default, <VBox user home>/vmi/<VM UUID>.sock. */
    rc = CFGMR3QueryStringAlloc(pCfg, "SocketPath", &pThis->pszPath);
    if (RT_FAILURE(rc))
        rc = VM_SET_ERROR_U(pUVM, rc, "Configuration error: Failed querying \"VMI/SocketPath\"");
    if (RT_SUCCESS(rc))
        rc = RTSemFastMutexCreate(&pThis->hMtxClient);
    if (RT_SUCCESS(rc))
        rc = vmiR3Listen(pThis);
    if (RT_SUCCESS(rc) && pipe2(pThis->afdWakeup, O_CLOEXEC) != 0)
        rc = VM_SET_ERROR_U(pUVM, RTErrConvertFromErrno(errno), "VMI: pipe2 failed");
    if (RT_SUCCESS(rc))
    {
        rc = VMR3AtStateRegister(pUVM, vmiR3AtState, pThis);
        pThis->fAtStateRegistered = RT_SUCCESS(rc);
    }
    if (RT_SUCCESS(rc))
    {
        rc = RTThreadCreate(&pThis->hThread, vmiR3Thread, pThis, 0 /*cbStack*/, RTTHREADTYPE_IO,
                            RTTHREADFLAGS_WAITABLE, "VMI");
        if (RT_SUCCESS(rc))
        {
            LogRel(("VMI: Listening on %s\n", pThis->pszPath));
            *ppvVmi = pThis;
            return VINF_SUCCESS;
        }
        pThis->hThread = NIL_RTTHREAD;
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

    if (pThis->fAtStateRegistered)
    {
        int rc = VMR3AtStateDeregister(pUVM, vmiR3AtState, pThis);
        AssertRC(rc);
        pThis->fAtStateRegistered = false;
    }

    if (pThis->hThread != NIL_RTTHREAD)
    {
        char ch = 0;
        ssize_t cbWritten;
        do
            cbWritten = write(pThis->afdWakeup[1], &ch, 1);
        while (cbWritten < 0 && errno == EINTR);

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
    MMR3HeapFree(pThis->pszPath);
    RTMemFree(pThis);
}
