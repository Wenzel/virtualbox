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
#include <VBox/vmm/vm.h>
#include <VBox/vmm/uvm.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <iprt/assert.h>
#include <iprt/dir.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/thread.h>

#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>


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
    /** The connected client, -1 if none. */
    int             fdClient;
    /** Pipe for waking up the server thread on termination: [0] read, [1] write. */
    int             afdWakeup[2];
    /** Set when the client completed the handshake. */
    bool            fHandshaked;
    /** The socket path (heap). */
    char           *pszPath;
    /** Message buffer, sized so that larger messages are detected (MSG_TRUNC). */
    uint8_t         abMsg[VMI_MSG_SIZE_MAX];
} VMIR3;
/** Pointer to the VBoxVMI server instance. */
typedef VMIR3 *PVMIR3;


/**
 * Sends a message to a socket.
 *
 * @returns VBox status code.
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
    uint8_t   abMsg[sizeof(VMIMSGHDR) + sizeof(VMIHELLO)];
    AssertReturn(cbBody <= sizeof(abMsg) - sizeof(VMIMSGHDR), VERR_BUFFER_OVERFLOW);
    VMIMSGHDR Hdr;
    Hdr.cbMsg  = (uint32_t)(sizeof(Hdr) + cbBody);
    Hdr.idReq  = idReq;
    Hdr.uType  = uType;
    Hdr.fFlags = fFlags;
    Hdr.rc     = rcMsg;
    memcpy(abMsg, &Hdr, sizeof(Hdr));
    if (cbBody)
        memcpy(&abMsg[sizeof(Hdr)], pvBody, cbBody);
    ssize_t cbSent = send(fd, abMsg, Hdr.cbMsg, MSG_NOSIGNAL);
    if (cbSent == (ssize_t)Hdr.cbMsg)
        return VINF_SUCCESS;
    return cbSent < 0 ? RTErrConvertFromErrno(errno) : VERR_NET_PROTOCOL_ERROR;
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
 * Drops the connected client.
 */
static void vmiR3Disconnect(PVMIR3 pThis)
{
    LogRel(("VMI: Client disconnected\n"));
    close(pThis->fdClient);
    pThis->fdClient    = -1;
    pThis->fHandshaked = false;
}


/**
 * Accepts a pending connection: it becomes the client if it is the only one
 * and runs as our user, otherwise it is told why and dropped.
 */
static void vmiR3Accept(PVMIR3 pThis)
{
    int fd = accept4(pThis->fdListen, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0)
    {
        LogRel(("VMI: accept failed: errno=%d\n", errno));
        return;
    }

    int rcGreeting = VINF_SUCCESS;
    struct ucred Cred;
    socklen_t    cbCred = sizeof(Cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &Cred, &cbCred) != 0)
        rcGreeting = RTErrConvertFromErrno(errno);
    else if (Cred.uid != geteuid())
    {
        LogRel(("VMI: Refused client pid %d: uid %u is not ours\n", Cred.pid, Cred.uid));
        rcGreeting = VERR_ACCESS_DENIED;
    }
    else if (pThis->fdClient >= 0)
    {
        LogRel(("VMI: Refused client pid %d: another client is connected\n", Cred.pid));
        rcGreeting = VERR_RESOURCE_BUSY;
    }

    int rc = vmiR3SendHello(fd, 0 /*idReq*/, 0 /*fFlags*/, rcGreeting);
    if (RT_SUCCESS(rc) && RT_SUCCESS(rcGreeting))
    {
        LogRel(("VMI: Client pid %d connected\n", Cred.pid));
        pThis->fdClient    = fd;
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
    if (cbBody != sizeof(VMIHELLO))
        return false;
    if (pThis->fHandshaked)
        return RT_SUCCESS(vmiR3SendHello(pThis->fdClient, pHdr->idReq, VMI_MSG_F_REPLY, VERR_WRONG_ORDER));

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
 * Receives and handles one message from the client.
 *
 * @returns false if the client must be disconnected.
 */
static bool vmiR3HandleMsg(PVMIR3 pThis)
{
    /* MSG_TRUNC makes recv return the real size of an oversized message. */
    ssize_t cbMsg = recv(pThis->fdClient, pThis->abMsg, sizeof(pThis->abMsg), MSG_TRUNC);
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
        struct pollfd aFds[3];
        aFds[0].fd = pThis->afdWakeup[0];
        aFds[1].fd = pThis->fdListen;
        aFds[2].fd = pThis->fdClient; /* ignored by poll when -1 */
        for (unsigned i = 0; i < RT_ELEMENTS(aFds); i++)
        {
            aFds[i].events  = POLLIN;
            aFds[i].revents = 0;
        }

        int cReady = poll(aFds, RT_ELEMENTS(aFds), -1);
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

    /* The default path's directory (<VBox user home>/vmi) may not exist yet. */
    char szDir[RTPATH_MAX];
    RTStrCopy(szDir, sizeof(szDir), pThis->pszPath);
    RTPathStripFilename(szDir);
    rc = RTDirCreateFullPath(szDir, 0700);
    if (RT_FAILURE(rc))
        return VMR3SetError(pUVM, rc, RT_SRC_POS, "VMI: Failed to create the directory \"%s\"", szDir);

    /* A socket left behind by a VM process that crashed would make bind fail; anything else is not ours to remove. */
    struct stat St;
    if (lstat(pThis->pszPath, &St) == 0)
    {
        if (!S_ISSOCK(St.st_mode))
            return VMR3SetError(pUVM, VERR_ALREADY_EXISTS, RT_SRC_POS,
                                "VMI: \"%s\" exists and is not a socket", pThis->pszPath);
        unlink(pThis->pszPath);
    }

    pThis->fdListen = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (pThis->fdListen < 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS, "VMI: socket failed (errno=%d)", errno);
    if (bind(pThis->fdListen, (struct sockaddr *)&Addr, sizeof(Addr)) != 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS,
                            "VMI: Failed to bind \"%s\" (errno=%d)", pThis->pszPath, errno);
    /* Before listen, so nobody can connect while the umask derived mode is in effect. */
    if (chmod(pThis->pszPath, 0600) != 0)
        return VMR3SetError(pUVM, RTErrConvertFromErrno(errno), RT_SRC_POS,
                            "VMI: Failed to chmod \"%s\" (errno=%d)", pThis->pszPath, errno);
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
    pThis->afdWakeup[0] = -1;
    pThis->afdWakeup[1] = -1;

    /* Main sets the default, <VBox user home>/vmi/<VM UUID>.sock. */
    rc = CFGMR3QueryStringAlloc(pCfg, "SocketPath", &pThis->pszPath);
    if (RT_FAILURE(rc))
        rc = VM_SET_ERROR_U(pUVM, rc, "Configuration error: Failed querying \"VMI/SocketPath\"");
    if (RT_SUCCESS(rc))
        rc = vmiR3Listen(pThis);
    if (RT_SUCCESS(rc) && pipe2(pThis->afdWakeup, O_CLOEXEC) != 0)
        rc = VM_SET_ERROR_U(pUVM, RTErrConvertFromErrno(errno), "VMI: pipe2 failed");
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
        rc = VM_SET_ERROR_U(pUVM, rc, "VMI: Failed to create the server thread");
    }

    VMIR3Terminate(pUVM, pThis);
    return rc;
}


/**
 * Stops the VBoxVMI server, disconnecting the client and removing the socket.
 *
 * @param   pUVM        The user mode VM handle.
 * @param   pvVmi       The server instance from VMIR3Create, NULL is fine.
 */
VMMR3_INT_DECL(void) VMIR3Terminate(PUVM pUVM, void *pvVmi)
{
    RT_NOREF(pUVM);
    PVMIR3 pThis = (PVMIR3)pvVmi;
    if (!pThis)
        return;

    if (pThis->hThread != NIL_RTTHREAD)
    {
        char ch = 0;
        ssize_t cbIgn = write(pThis->afdWakeup[1], &ch, 1); RT_NOREF(cbIgn);
        int rc = RTThreadWait(pThis->hThread, RT_INDEFINITE_WAIT, NULL);
        AssertLogRelRC(rc);
    }
    if (pThis->fdListen >= 0)
    {
        close(pThis->fdListen);
        unlink(pThis->pszPath);
    }
    if (pThis->afdWakeup[0] >= 0)
        close(pThis->afdWakeup[0]);
    if (pThis->afdWakeup[1] >= 0)
        close(pThis->afdWakeup[1]);
    MMR3HeapFree(pThis->pszPath);
    RTMemFree(pThis);
}
