#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# $Id$
"""
Checks the IMachineDebugger memory methods: readPhysicalMemory and
writePhysicalMemory against guest core dumps taken with
IMachineDebugger::dumpGuestCore, readVirtualMemory and writeVirtualMemory
against a page table walk of the script's own.

Usage: tstMachineDebuggerMemory.py <running-vm-name> [scratch-dir]

The VM must be running.  The script reads guest RAM while it runs, then pauses
it and compares RAM pages with a core dump, writes a pattern, checks it in a
fresh core dump, restores the original bytes and resumes the VM.  It also
checks that MMIO and unassigned addresses fail cleanly.  The virtual memory
methods are checked on vCPU 0 at RIP and the IDT base, translated by walking
the guest's long mode page tables, including a write thru the guest's mapping
of the local APIC (if it has one), which must fail and leave the zero pages
alone, and a non-canonical address, which must fail.  The core
dumps (about the size of guest RAM) are written to scratch-dir and deleted
again.

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
X86_PAGE_MASK = 0x000ffffffffff000
X86_PTE_P = 0x1
X86_PDE_PS = 0x80
X86_CR4_LA57 = 0x1000


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


def toLong64(GCPtr):
    """ The API takes addresses as signed 64-bit integers, kernel addresses are negative. """
    return GCPtr - (1 << 64) if GCPtr >= 1 << 63 else GCPtr


def zeroPages(sCore):
    """ Counts the guest RAM pages in a core file that are all zeros. """
    abZero = bytes(PAGE)
    cZero = 0
    with open(sCore, 'rb') as oFile:
        for _, off, cbSeg in ramSegments(sCore):
            oFile.seek(off)
            for _ in range(cbSeg // PAGE):
                cZero += oFile.read(PAGE) == abZero
    return cZero


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

    def readVirt(GCPtr, cb, idCpu=0):
        return bytes(oDbg.readVirtualMemory(idCpu, toLong64(GCPtr), cb))

    def writeVirt(GCPtr, abData, idCpu=0):
        oDbg.writeVirtualMemory(idCpu, toLong64(GCPtr), len(abData), list(abData))

    def pagingRoot(idCpu=0):
        """ Returns the top level page table address and the number of levels (long mode only). """
        if not int(oDbg.getRegister(idCpu, 'efer'), 16) & 0x400: # EFER.LMA
            raise Exception('vCPU %u is not in long mode, the only paging mode this script walks' % (idCpu,))
        cLevels = 5 if int(oDbg.getRegister(idCpu, 'cr4'), 16) & X86_CR4_LA57 else 4
        return int(oDbg.getRegister(idCpu, 'cr3'), 16) & X86_PAGE_MASK, cLevels

    def translate(GCPtr, idCpu=0):
        """ Walks the guest page tables with readPhysicalMemory, returns the GCPhys or None if not present. """
        GCPhysTable, cLevels = pagingRoot(idCpu)
        for iLevel in range(cLevels, 0, -1):
            cShift = 12 + 9 * (iLevel - 1)
            uEntry, = struct.unpack('<Q', read(GCPhysTable + ((GCPtr >> cShift) & 0x1ff) * 8, 8))
            if not uEntry & X86_PTE_P:
                return None
            if iLevel == 1 or (iLevel in (2, 3) and uEntry & X86_PDE_PS):
                fOffMask = (1 << cShift) - 1
                return (uEntry & X86_PAGE_MASK & ~fOffMask) | (GCPtr & fOffMask)
            GCPhysTable = uEntry & X86_PAGE_MASK
        return None

    def findMapping(GCPhysPage, idCpu=0):
        """ Returns the highest kernel (upper half) address mapping the given guest physical page, or None.
            Walks downwards, as the fixmap area is at the top. """
        GCPhysRoot, cLevels = pagingRoot(idCpu)
        def walk(GCPhysTable, iLevel, GCPtrBase):
            cShift = 12 + 9 * (iLevel - 1)
            abTable = read(GCPhysTable, PAGE)
            for iEntry in range(511, 255 if iLevel == cLevels else -1, -1):
                uEntry, = struct.unpack_from('<Q', abTable, iEntry * 8)
                GCPtr = GCPtrBase | (iEntry << cShift)
                if not uEntry & X86_PTE_P:
                    continue
                if iLevel == 1 or (iLevel in (2, 3) and uEntry & X86_PDE_PS):
                    GCPhys = uEntry & X86_PAGE_MASK & ~((1 << cShift) - 1)
                    if GCPhys <= GCPhysPage < GCPhys + (1 << cShift):
                        return GCPtr + GCPhysPage - GCPhys
                else:
                    GCPtr = walk(uEntry & X86_PAGE_MASK, iLevel - 1, GCPtr)
                    if GCPtr is not None:
                        return GCPtr
            return None
        GCPtr = walk(GCPhysRoot, cLevels, 0)
        return None if GCPtr is None else GCPtr | ~((1 << (12 + 9 * cLevels)) - 1) & 0xffffffffffffffff # canonical

    def skip(sWhat, sWhy):
        print('SKIPPED: %s: %s' % (sWhat, sWhy))

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

        # Virtual memory on vCPU 0, paused: compare with readPhysicalMemory at the addresses our own
        # page table walk gives, for RIP (kernel text, 3 pages + 5 bytes) and the IDT (one page).
        for sReg, cb in (('rip', 3 * PAGE + 5), ('idtr_base', PAGE)):
            GCPtrPage = int(oDbg.getRegister(0, sReg), 16) & ~(PAGE - 1)
            aGCPhys = [translate(GCPtrPage + off) for off in range(0, cb, PAGE)]
            check(None not in aGCPhys, '%s page %#x translates to %s' % (sReg, GCPtrPage, ', '.join('%#x' % (GCPhys or 0,)
                                                                                                for GCPhys in aGCPhys)))
            if None in aGCPhys:
                continue
            abPhys = b''.join(read(GCPhys, PAGE) for GCPhys in aGCPhys)[:cb]
            check(readVirt(GCPtrPage, cb) == abPhys, 'readVirtualMemory of %#x bytes at %s page %#x matches the physical pages'
                  % (cb, sReg, GCPtrPage))

        # Write a pattern thru the (read-only) kernel text mapping of RIP, check it via the physical
        # address and in a fresh core dump, then restore.
        GCPtr = int(oDbg.getRegister(0, 'rip'), 16) & ~(PAGE - 1)
        GCPhys = translate(GCPtr)
        abOrg = readVirt(GCPtr, PAGE)
        if GCPhys is None:
            check(False, 'RIP page %#x translates' % (GCPtr,))
        else:
            abPattern = os.urandom(PAGE)
            writeVirt(GCPtr, abPattern)
            try:
                check(read(GCPhys, PAGE) == abPattern, 'writeVirtualMemory pattern at %#x read back at %#x' % (GCPtr, GCPhys))
                sCore = dumpCore('tstMachineDebuggerMemory-3.core')
                try:
                    check(coreRead(sCore, ramSegments(sCore), GCPhys, PAGE) == abPattern,
                          'writeVirtualMemory pattern in fresh core at %#x' % (GCPhys,))
                finally:
                    os.remove(sCore)
            finally:
                writeVirt(GCPtr, abOrg)
            check(read(GCPhys, PAGE) == abOrg, 'original bytes restored at %#x' % (GCPtr,))
        GCPtrText, GCPhysText = GCPtr, GCPhys # kernel text, for the read while running below

        # Non-canonical alias of the kernel text page (bit 48 flipped, same page table indexes): must fail.
        GCPtrNonCanonical = GCPtr ^ (1 << 48)
        fails(lambda: readVirt(GCPtrNonCanonical, 16), 'virtual read of non-canonical %#x' % (GCPtrNonCanonical,))
        fails(lambda: writeVirt(GCPtrNonCanonical, abOrg[:4]), 'virtual write of non-canonical %#x' % (GCPtrNonCanonical,))

        # Unmapped address, bad CPU id: clean errors.
        GCPtrUnmapped = 0
        check(translate(GCPtrUnmapped) is None, 'address %#x is not mapped' % (GCPtrUnmapped,))
        fails(lambda: readVirt(GCPtrUnmapped, 16), 'virtual read of unmapped %#x' % (GCPtrUnmapped,))
        fails(lambda: writeVirt(GCPtrUnmapped, b'\xff' * 4), 'virtual write of unmapped %#x' % (GCPtrUnmapped,))
        idCpuBad = oMachine.CPUCount
        fails(lambda: readVirt(GCPtr, 16, idCpuBad), 'virtual read on CPU %u' % (idCpuBad,))
        fails(lambda: writeVirt(GCPtr, abOrg[:4], idCpuBad), 'virtual write on CPU %u' % (idCpuBad,))

        # The guest's own mapping of the local APIC: the APIC access page is aliased to the zero page
        # when the APIC is virtualised, so a write going thru must not modify every zero page.
        # (A physical write at 0xfee00000 is checked above regardless.)
        GCPtrApic = findMapping(0xfee00000)
        if GCPtrApic is None:
            skip('virtual write thru the local APIC mapping', 'the guest does not map 0xfee00000 (x2APIC mode?)')
        else:
            print('Guest maps the local APIC at %#x' % (GCPtrApic,))
            sCore = dumpCore('tstMachineDebuggerMemory-4.core')
            cZeroBefore = zeroPages(sCore)
            os.remove(sCore)
            fails(lambda: readVirt(GCPtrApic, 16), 'virtual read of the local APIC at %#x' % (GCPtrApic,))
            fails(lambda: writeVirt(GCPtrApic, b'\xff' * 4), 'virtual write of the local APIC at %#x' % (GCPtrApic,))
            sCore = dumpCore('tstMachineDebuggerMemory-4.core')
            cZeroAfter = zeroPages(sCore)
            os.remove(sCore)
            check(cZeroAfter == cZeroBefore, 'zero pages unchanged by the local APIC write (%u before, %u after)'
                  % (cZeroBefore, cZeroAfter))
        oConsole.resume()
        check(oMachine.state == oConsts.MachineState_Running, 'VM resumed')

        # Running: a virtual read of the kernel text page translated while paused (its mapping and
        # content are stable while the guest runs).
        if GCPhysText is not None:
            check(readVirt(GCPtrText, PAGE) == read(GCPhysText, PAGE),
                  'readVirtualMemory of kernel text %#x while running' % (GCPtrText,))
    finally:
        if oMachine.state == oConsts.MachineState_Paused:
            oConsole.resume()
        oSession.unlockMachine()
    print('%u error(s)' % (cErrors,))
    return 1 if cErrors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
