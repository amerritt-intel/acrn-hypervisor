/*
 * Copyright (c) 2011 NetApp, Inc.
 * Copyright (c) 2018 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <vm.h>
#include <ptdev.h>
#include <assign.h>
#include <vpci.h>
#include "vpci_priv.h"


/**
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 * @pre vdev->vpci->vm != NULL
 * @pre vdev->pdev != NULL
 */
static int32_t vmsi_remap(const struct pci_vdev *vdev, bool enable)
{
	struct ptirq_msi_info info;
	union pci_bdf pbdf = vdev->pdev->bdf;
	struct acrn_vm *vm = vdev->vpci->vm;
	uint32_t capoff = vdev->msi.capoff;
	uint32_t msgctrl, msgdata;
	uint32_t addrlo, addrhi;
	int32_t ret;

	/* Disable MSI during configuration */
	msgctrl = pci_vdev_read_cfg(vdev, capoff + PCIR_MSI_CTRL, 2U);
	if ((msgctrl & PCIM_MSICTRL_MSI_ENABLE) == PCIM_MSICTRL_MSI_ENABLE) {
		pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_CTRL, 2U, msgctrl & ~PCIM_MSICTRL_MSI_ENABLE);
	}

	/* Read the MSI capability structure from virtual device */
	addrlo = pci_vdev_read_cfg_u32(vdev, capoff + PCIR_MSI_ADDR);
	if ((msgctrl & PCIM_MSICTRL_64BIT) != 0U) {
		msgdata = pci_vdev_read_cfg_u16(vdev, capoff + PCIR_MSI_DATA_64BIT);
		addrhi = pci_vdev_read_cfg_u32(vdev, capoff + PCIR_MSI_ADDR_HIGH);
	} else {
		msgdata = pci_vdev_read_cfg_u16(vdev, capoff + PCIR_MSI_DATA);
		addrhi = 0U;
	}

	info.vmsi_addr.full = (uint64_t)addrlo | ((uint64_t)addrhi << 32U);

	/* MSI is being enabled or disabled */
	if (enable) {
		info.vmsi_data.full = msgdata;
	} else {
		info.vmsi_data.full = 0U;
	}

	ret = ptirq_msix_remap(vm, vdev->bdf.value, pbdf.value, 0U, &info);
	if (ret == 0) {
		/* Update MSI Capability structure to physical device */
		if ((msgctrl & PCIM_MSICTRL_64BIT) != 0U) {
			pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_DATA_64BIT, 0x2U, (uint16_t)info.pmsi_data.full);
		} else {
			pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_DATA, 0x2U, (uint16_t)info.pmsi_data.full);
		}

		if (enable) {
			pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_ADDR, 0x4U, (uint32_t)info.pmsi_addr.full);
			if ((msgctrl & PCIM_MSICTRL_64BIT) != 0U) {
				pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_ADDR_HIGH, 0x4U,
					(uint32_t)(info.pmsi_addr.full >> 32U));
			}

			/* If MSI Enable is being set, make sure INTxDIS bit is set */
			enable_disable_pci_intx(pbdf, false);
			pci_pdev_write_cfg(pbdf, capoff + PCIR_MSI_CTRL, 2U, msgctrl | PCIM_MSICTRL_MSI_ENABLE);
		}
	}

	return ret;
}

/**
 * @pre vdev != NULL
 */
void vmsi_read_cfg(const struct pci_vdev *vdev, uint32_t offset, uint32_t bytes, uint32_t *val)
{
	/* For PIO access, we emulate Capability Structures only */
	*val = pci_vdev_read_cfg(vdev, offset, bytes);
}

/**
 * @brief Writing MSI Capability Structure
 *
 * @pre vdev != NULL
 */
void vmsi_write_cfg(struct pci_vdev *vdev, uint32_t offset, uint32_t bytes, uint32_t val)
{
	bool message_changed = false;
	bool enable;
	uint32_t msgctrl;

	/* Save msgctrl for comparison */
	msgctrl = pci_vdev_read_cfg(vdev, vdev->msi.capoff + PCIR_MSI_CTRL, 2U);

	/* Either Message Data or message Addr is being changed */
	if (((offset - vdev->msi.capoff) >= PCIR_MSI_ADDR) && (val != pci_vdev_read_cfg(vdev, offset, bytes))) {
		message_changed = true;
	}

	/* Write to vdev */
	pci_vdev_write_cfg(vdev, offset, bytes, val);

	/* Do remap if MSI Enable bit is being changed */
	if (((offset - vdev->msi.capoff) == PCIR_MSI_CTRL) &&
		(((msgctrl ^ val) & PCIM_MSICTRL_MSI_ENABLE) != 0U)) {
		enable = ((val & PCIM_MSICTRL_MSI_ENABLE) != 0U);
		(void)vmsi_remap(vdev, enable);
	} else {
		if (message_changed && ((msgctrl & PCIM_MSICTRL_MSI_ENABLE) != 0U)) {
			(void)vmsi_remap(vdev, true);
		}
	}
}

/**
 * @pre vdev != NULL
 * @pre vdev->vpci != NULL
 * @pre vdev->vpci->vm != NULL
 */
void deinit_vmsi(const struct pci_vdev *vdev)
{
	if (has_msi_cap(vdev)) {
		ptirq_remove_msix_remapping(vdev->vpci->vm, vdev->bdf.value, 1U);
	}
}

/**
 * @pre vdev != NULL
 * @pre vdev->pdev != NULL
 */
void init_vmsi(struct pci_vdev *vdev)
{
	struct pci_pdev *pdev = vdev->pdev;
	uint32_t val;

	vdev->msi.capoff = pdev->msi_capoff;

	if (has_msi_cap(vdev)) {
		val = pci_pdev_read_cfg(pdev->bdf, vdev->msi.capoff, 4U);
		vdev->msi.caplen = ((val & (PCIM_MSICTRL_64BIT << 16U)) != 0U) ? 14U : 10U;

		val &= ~((uint32_t)PCIM_MSICTRL_MMC_MASK << 16U);
		val &= ~((uint32_t)PCIM_MSICTRL_MME_MASK << 16U);
		pci_vdev_write_cfg(vdev, vdev->msi.capoff, 4U, val);
	}
}

