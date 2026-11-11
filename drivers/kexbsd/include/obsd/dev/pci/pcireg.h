/*
 *  KernelExEx: OpenBSD <dev/pci/pcireg.h>, the PCI configuration space
 *  layout (PCI Local Bus and PCI Express specifications).
 */
#ifndef KEXBSD_OBSD_PCIREG_H
#define KEXBSD_OBSD_PCIREG_H

#include "obsd/kexobsd.h"

#define PCI_ID_REG 0x00
#define PCI_VENDOR(id) ((id) & 0xffff)
#define PCI_PRODUCT(id) (((id) >> 16) & 0xffff)
#define PCI_COMMAND_STATUS_REG 0x04
#define PCI_COMMAND_IO_ENABLE 0x00000001
#define PCI_COMMAND_MEM_ENABLE 0x00000002
#define PCI_COMMAND_MASTER_ENABLE 0x00000004
#define PCI_COMMAND_INVALIDATE_ENABLE 0x00000010
#define PCI_STATUS_CAPLIST_SUPPORT 0x00100000
#define PCI_CLASS_REG 0x08
#define PCI_REVISION(cr) ((cr) & 0xff)
#define PCI_CLASS(cr) (((cr) >> 24) & 0xff)
#define PCI_SUBCLASS(cr) (((cr) >> 16) & 0xff)
#define PCI_BHLC_REG 0x0c
#define PCI_SUBSYS_ID_REG 0x2c
#define PCI_CAPLISTPTR_REG 0x34
#define PCI_INTERRUPT_REG 0x3c

#define PCI_MAPREG_START 0x10
#define PCI_MAPREG_END 0x28
#define PCI_MAPREG_TYPE(mr) ((mr) & 0x00000001)
#define PCI_MAPREG_TYPE_MEM 0x00000000
#define PCI_MAPREG_TYPE_IO 0x00000001
#define PCI_MAPREG_MEM_TYPE(mr) ((mr) & 0x00000006)
#define PCI_MAPREG_MEM_TYPE_32BIT 0x00000000
#define PCI_MAPREG_MEM_TYPE_32BIT_1M 0x00000002
#define PCI_MAPREG_MEM_TYPE_64BIT 0x00000004
#define PCI_MAPREG_MEM_ADDR(mr) ((mr) & 0xfffffff0)
#define PCI_MAPREG_IO_ADDR(mr) ((mr) & 0xfffffffc)

#define PCI_CAP_PWRMGMT 0x01
#define PCI_CAP_MSI 0x05
#define PCI_CAP_VENDSPEC 0x09
#define PCI_CAP_PCIEXPRESS 0x10
#define PCI_CAP_MSIX 0x11

/* PCI Express capability registers, from the capability's offset */
#define PCI_PCIE_XCAP 0x00
#define PCI_PCIE_DCAP 0x04
#define PCI_PCIE_DCSR 0x08
#define PCI_PCIE_LCAP 0x0c
#define PCI_PCIE_LCSR 0x10
#define PCI_PCIE_LCSR_ASPM_L0S 0x00000001
#define PCI_PCIE_LCSR_ASPM_L1 0x00000002
#define PCI_PCIE_DCSR2 0x28

#endif
