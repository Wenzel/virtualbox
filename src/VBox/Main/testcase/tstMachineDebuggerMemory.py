#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# $Id$
"""
Checks IMachineDebugger::readPhysicalMemory and writePhysicalMemory against
guest core dumps taken with IMachineDebugger::dumpGuestCore.

Usage: tstMachineDebuggerMemory.py <running-vm-name> [scratch-dir]

The VM must be running.  The script reads guest RAM while it runs, then pauses
it and compares RAM pages with a core dump, writes a pattern, checks it in a
fresh core dump, restores the original bytes and resumes the VM.  It also
checks that MMIO and unassigned addresses fail cleanly.  The two core dumps
(about the size of guest RAM) are written to scratch-dir and deleted again.

Running against a build output directory without installing, e.g.:
    export VBOX_PROGRAM_PATH=<out>/bin/ VBOX_SDK_PATH=<out>/bin/sdk/
    export PYTHONPATH=<out>/bin/sdk/installer/python/vboxapi/src:<out>/bin
    python3 tstMachineDebuggerMemory.py myvm /tmp
"""

__copyright__ = \
"""
Copyright (C) 2026 Oracle and/or its affiliates.

This file is part of VirtualBox base platform packages, as
available from https://www.virtualbox.org.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, in version 3 of the
License.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <https://www.gnu.org/licenses>.

SPDX-License-Identifier: GPL-3.0-only
"""

import os
import re
import struct
import sys
import time

from vboxapi import VirtualBoxManager

PAGE = 0x1000


def ramSegments(sCore):
    """ Returns (GCPhys, file offset, size) of each RAM PT_LOAD segment of a VBox ELF core. """
    with open(sCore, 'rb') as oFile:
        abHdr = oFile.read(64)
        assert abHdr[:4] == b'\x7fELF', sCore
        offPh, = struct.unpack_from('<Q', abHdr, 0x20)
        cbPh, cPh = struct.unpack_from('<HH', abHdr, 0x36)
        oFile.seek(offPh)
        abPh = oFile.read(cbPh * cPh)
    aSegs = []
    for i in range(cPh):
        uType, _, off, _, GCPhys, cbFile, _, _ = struct.unpack_from('<IIQQQQQQ', abPh, i * cbPh)
        if uType == 1 and cbFile: # PT_LOAD, MMIO ranges have no file data.
            aSegs.append((GCPhys, off, cbFile))
    return aSegs


def coreRead(sCore, aSegs, GCPhys, cb):
    """ Reads cb bytes at GCPhys from a core file. """
    for GCPhysSeg, off, cbSeg in aSegs:
        if GCPhysSeg <= GCPhys and GCPhys + cb <= GCPhysSeg + cbSeg:
            with open(sCore, 'rb') as oFile:
                oFile.seek(off + GCPhys - GCPhysSeg)
                return oFile.read(cb)
    raise Exception('%#x not in core' % (GCPhys,))


def main(asArgs):
    sVm = asArgs[1]
    sDir = asArgs[2] if len(asArgs) > 2 else '.'
    oMgr = VirtualBoxManager(None, None)
    oConsts = oMgr.constants
    oMachine = oMgr.getVirtualBox().findMachine(sVm)
    oSession = oMgr.getSessionObject()
    oMachine.lockMachine(oSession, oConsts.LockType_Shared)
    oConsole = oSession.console
    oDbg = oConsole.debugger
    cErrors = 0

    def check(fOk, sWhat):
        nonlocal cErrors
        print('%s: %s' % ('PASSED' if fOk else 'FAILED', sWhat))
        cErrors += not fOk

    def read(GCPhys, cb):
        return bytes(oDbg.readPhysicalMemory(GCPhys, cb))

    def write(GCPhys, abData):
        oDbg.writePhysicalMemory(GCPhys, len(abData), list(abData))

    def fails(fnAccess, sWhat):
        try:
            fnAccess()
        except Exception as oXcpt: # pylint: disable=broad-except
            check(True, '%s fails: %s' % (sWhat, oMgr.xcptGetMessage(oXcpt)))
            return
        check(False, '%s should fail' % (sWhat,))

    def executingPeriods():
        return int(re.search(r'val="(\d+)"', oDbg.getStats('/TM/CPU/00/cPeriodsExecuting', False)).group(1))

    def dumpCore(sName):
        sCore = os.path.abspath(os.path.join(sDir, sName))
        if os.path.exists(sCore):
            os.remove(sCore)
        oDbg.dumpGuestCore(sCore, '')
        return sCore

    def patternTest(GCPhys, fDump):
        abOrg = read(GCPhys, PAGE)
        abPattern = os.urandom(PAGE)
        write(GCPhys, abPattern)
        try:
            check(read(GCPhys, PAGE) == abPattern, 'pattern read back at %#x' % (GCPhys,))
            if fDump:
                sCore = dumpCore('tstMachineDebuggerMemory-2.core')
                try:
                    check(coreRead(sCore, ramSegments(sCore), GCPhys, PAGE) == abPattern,
                          'pattern in fresh core at %#x' % (GCPhys,))
                finally:
                    os.remove(sCore)
        finally:
            write(GCPhys, abOrg)
        check(read(GCPhys, PAGE) == abOrg, 'original bytes restored at %#x' % (GCPhys,))

    # The guest physical memory map: [(first, last, description)].
    aPhysMap = [(int(oMatch.group(1), 16), int(oMatch.group(2), 16), oMatch.group(3))
                for oMatch in re.finditer(r'^([0-9a-f]{16})-([0-9a-f]{16}) [0-9a-f]{16} (.*)$', oDbg.info('phys', ''), re.M)]

    try:
        check(oMachine.state == oConsts.MachineState_Running, 'VM is running')

        # Running: reads, and a write/restore on the first VRAM page.  Unlike any RAM page, the guest
        # kernel keeps no state there, so a guest write racing with the test costs at most some pixels.
        check(len(read(0x100000, 3 * PAGE + 5)) == 3 * PAGE + 5, 'read 3 pages + 5 bytes at 1MB while running')
        GCPhysVRam = [aRange[0] for aRange in aPhysMap if aRange[2] == 'VRam'][0]
        patternTest(GCPhysVRam, False)

        # MMIO and unassigned memory: clean errors, the access never reaches a device.
        for GCPhys, sDesc in ((0xfee00000, 'local APIC'), (0xfec00000, 'I/O APIC'), (0x7fff00000000, 'unassigned')):
            fails(lambda GCPhys=GCPhys: read(GCPhys, 16), 'read of %s at %#x' % (sDesc, GCPhys))
            fails(lambda GCPhys=GCPhys: write(GCPhys, b'\xff' * 4), 'write of %s at %#x' % (sDesc, GCPhys))
        cRuns = executingPeriods()
        time.sleep(0.5)
        check(oMachine.state == oConsts.MachineState_Running and executingPeriods() > cRuns,
              'VM still running and executing guest code after the MMIO accesses')

        # Paused: compare with a core dump, then the pattern test with a fresh one.
        oConsole.pause()
        sCore = dumpCore('tstMachineDebuggerMemory-1.core')
        aSegs = ramSegments(sCore)
        print('RAM segments in the core: %s' % (', '.join('%#x-%#x' % (s[0], s[0] + s[2] - 1) for s in aSegs),))
        cbTotal = cBad = 0
        for GCPhysSeg, _, cbSeg in aSegs:
            for GCPhys in range(GCPhysSeg, GCPhysSeg + cbSeg, 0x100000):
                cb = min(0x100000, GCPhysSeg + cbSeg - GCPhys)
                if read(GCPhys, cb) != coreRead(sCore, aSegs, GCPhys, cb):
                    print('mismatch in %#x LB %#x' % (GCPhys, cb))
                    cBad += 1
                cbTotal += cb
        check(cBad == 0, 'all %#x bytes of the core match' % (cbTotal,))
        os.remove(sCore)
        # Top of guest RAM: the last page of the highest 'RAM' range; together they must add up to the
        # configured RAM size, less the legacy 0xa0000-0xfffff hole.
        aRam = [aRange for aRange in aPhysMap if 'RAM' in aRange[2]]
        cbRam = sum(aRange[1] - aRange[0] + 1 for aRange in aRam)
        check(oMachine.memorySize * 0x100000 - cbRam in (0, 0x60000),
              'RAM ranges (%#x bytes) match the configured %u MB' % (cbRam, oMachine.memorySize))
        GCPhysTop = max(aRange[1] for aRange in aRam) + 1 - PAGE
        for GCPhys in (0, GCPhysTop):
            patternTest(GCPhys, True)
        oConsole.resume()
        check(oMachine.state == oConsts.MachineState_Running, 'VM resumed')
    finally:
        if oMachine.state == oConsts.MachineState_Paused:
            oConsole.resume()
        oSession.unlockMachine()
    print('%u error(s)' % (cErrors,))
    return 1 if cErrors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
