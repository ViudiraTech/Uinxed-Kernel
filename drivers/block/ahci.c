#include <chipset/common.h>
#include <drivers/ahci.h>
#include <drivers/pci.h>
#include <kernel/debug.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/hhdm.h>

static pci_finding_request_t ahci_pci_request = {
	.type = PCI_FOUND_CLASS,
	.req  = {
		.class_req = {
			.class_code = 0x010601,
		},
	},
};

ahci_device_t ahci_devices[AHCI_MAX_DEVICES];
int           ahci_device_count = 0;

static volatile HBA_MEM *hba_mem = 0;

typedef struct ahci_port
{
	volatile HBA_PORT *regs;
	uint8_t            port_num;
	HBA_CMD_HEADER    *cmd_list;
	HBA_FIS           *fis;
	HBA_CMD_TBL       *cmd_tbl;
	uint64_t           clb_phys;
	uint64_t           fb_phys;
	uint64_t           ct_phys;
} ahci_port_t;

static ahci_port_t ahci_ports[AHCI_MAX_PORTS];
static int         ahci_port_count  = 0;
#define AHCI_DMA_BUF_PAGES 8
#define AHCI_DMA_MAX_SECTORS (AHCI_DMA_BUF_PAGES * 4096 / 512)

static uint8_t *ahci_dma_buffer  = 0;
static uint64_t ahci_dma_buf_phys = 0;

static uint32_t ahci_read_reg(volatile uint32_t *reg)
{
	return mmio_read32((void *)reg);
}

static void ahci_write_reg(volatile uint32_t *reg, uint32_t value)
{
	mmio_write32((uint32_t *)reg, value);
}

static int ahci_port_find_slot(ahci_port_t *port)
{
	uint32_t slots = ((ahci_read_reg(&hba_mem->cap) >> 8) & 0x1F) + 1;
	uint32_t ci    = ahci_read_reg(&port->regs->ci);
	uint32_t act   = ahci_read_reg(&port->regs->sact);
	for (uint32_t i = 0; i < slots; i++) {
		if (!((ci | act) & (1u << i))) return (int)i;
	}
	return -1;
}

static void ahci_port_stop(ahci_port_t *port)
{
	int timeout;

	ahci_write_reg(&port->regs->cmd,
		ahci_read_reg(&port->regs->cmd) & ~(uint32_t)HBA_PxCMD_ST);
	timeout = 500000;
	while (ahci_read_reg(&port->regs->cmd) & HBA_PxCMD_CR) {
		if (--timeout <= 0) break;
	}

	ahci_write_reg(&port->regs->cmd,
		ahci_read_reg(&port->regs->cmd) & ~(uint32_t)HBA_PxCMD_FRE);
	timeout = 500000;
	while (ahci_read_reg(&port->regs->cmd) & HBA_PxCMD_FR) {
		if (--timeout <= 0) break;
	}
}

static int ahci_port_start(ahci_port_t *port)
{
	int timeout;

	ahci_port_stop(port);

	pointer_cast_t cast;

	cast.val = port->clb_phys;
	ahci_write_reg(&port->regs->clb, (uint32_t)(cast.val));
	cast.val = port->clb_phys >> 32;
	ahci_write_reg(&port->regs->clbu, (uint32_t)(cast.val));

	cast.val = port->fb_phys;
	ahci_write_reg(&port->regs->fb, (uint32_t)(cast.val));
	cast.val = port->fb_phys >> 32;
	ahci_write_reg(&port->regs->fbu, (uint32_t)(cast.val));

	ahci_write_reg(&port->regs->serr, 0xFFFFFFFF);

	ahci_write_reg(&port->regs->is, 0xFFFFFFFF);
	ahci_write_reg(&port->regs->ie, 0xFFFFFFFF);

	ahci_write_reg(&port->regs->cmd,
		ahci_read_reg(&port->regs->cmd) | HBA_PxCMD_FRE);
	timeout = 500000;
	while (!(ahci_read_reg(&port->regs->cmd) & HBA_PxCMD_FR)) {
		if (--timeout <= 0) return -ETIMEDOUT;
	}

	ahci_write_reg(&port->regs->cmd,
		ahci_read_reg(&port->regs->cmd) | HBA_PxCMD_ST);
	timeout = 500000;
	while (!(ahci_read_reg(&port->regs->cmd) & HBA_PxCMD_CR)) {
		if (--timeout <= 0) return -ETIMEDOUT;
	}

	return 0;
}

static int ahci_issue_cmd(ahci_port_t *port, int slot, uint8_t *cfis,
			  int write, uint64_t buf_phys, uint32_t byte_count)
{
	volatile HBA_CMD_HEADER *header = &port->cmd_list[slot];
	pointer_cast_t           cast;

	header->cfl   = (uint8_t)(sizeof(FIS_REG_H2D) / sizeof(uint32_t));
	header->w     = (uint8_t)(write ? 1 : 0);
	header->prdtl = (uint16_t)(byte_count ? 1 : 0);
	header->p     = 1;

	memcpy((void *)port->cmd_tbl->cfis, cfis, sizeof(FIS_REG_H2D));

	if (byte_count) {
		HBA_PRDT_ENTRY *prdt = &port->cmd_tbl->prdt_entry[0];
		cast.val          = buf_phys;
		prdt->dba         = (uint32_t)(cast.val & 0xFFFFFFFFULL);
		prdt->dbau        = (uint32_t)(cast.val >> 32);
		prdt->dbc         = byte_count - 1;
		prdt->i           = 1;
	}

	cast.val = port->ct_phys;
	header->ctba  = (uint32_t)(cast.val & 0xFFFFFFFFULL);
	cast.val = port->ct_phys >> 32;
	header->ctbau = (uint32_t)(cast.val);

	int timeout = 1000000;
	while (ahci_read_reg(&port->regs->tfd) & 0x88) {
		if (--timeout <= 0) return -EBUSY;
	}

	ahci_write_reg(&port->regs->ci, (uint32_t)(1 << slot));

	timeout = 1000000;
	while (1) {
		uint32_t ci = ahci_read_reg(&port->regs->ci);
		if (!(ci & (uint32_t)(1 << slot))) break;
		if (ahci_read_reg(&port->regs->is) & (1u << 30)) {
			ahci_write_reg(&port->regs->is, 1u << 30);
			return -EIO;
		}
		if (--timeout <= 0) return -ETIMEDOUT;
	}

	if (ahci_read_reg(&port->regs->tfd) & 0x01) return -EIO;
	return 0;
}

#define ATA_CMD_IDENTIFY_PACKET 0xA1

static int ahci_port_identify(ahci_port_t *port, ahci_device_t *dev)
{
	uint8_t  cmd = ATA_CMD_IDENTIFY;
	int      is_atapi = 0;

	uint32_t sig = ahci_read_reg(&port->regs->sig);
	if (sig == SATA_SIG_ATAPI) {
		cmd      = ATA_CMD_IDENTIFY_PACKET;
		is_atapi = 1;
	}

	FIS_REG_H2D cfis;
	memset(&cfis, 0, sizeof(cfis));
	cfis.fis_type = FIS_TYPE_REG_H2D;
	cfis.c        = 1;
	cfis.command  = cmd;
	cfis.device   = 0;

	int slot = ahci_port_find_slot(port);
	if (slot < 0) return -EBUSY;

	memset(ahci_dma_buffer, 0, 512);

	int ret = ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 0,
				 ahci_dma_buf_phys, 512);
	if (ret != 0) return ret;

	uint16_t *buf = (uint16_t *)ahci_dma_buffer;

	if (buf[0] == 0x0000 || buf[0] == 0xFFFF) return -ENODEV;

	dev->reserved    = 1;
	dev->type        = is_atapi ? AHCI_DEV_SATAPI : AHCI_DEV_SATA;
	dev->sector_size = 512;

	if (!is_atapi && (*((uint32_t *)(ahci_dma_buffer + 166)) & (1 << 26)))
		dev->size = *((uint32_t *)(ahci_dma_buffer + 200));
	else if (!is_atapi)
		dev->size = *((uint32_t *)(ahci_dma_buffer + 120));
	else
		dev->size = 0;

	for (int k = 0; k < 40; k += 2) {
		dev->model[k]     = ahci_dma_buffer[54 + k + 1];
		dev->model[k + 1] = ahci_dma_buffer[54 + k];
	}
	dev->model[40] = 0;

	for (int k = 39; k > 0; k--) {
		if (dev->model[k] == ' ') dev->model[k] = 0;
		else break;
	}

	return 0;
}

int ahci_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, void *buffer)
{
	if (drive >= AHCI_MAX_DEVICES || !ahci_devices[drive].reserved) return -ENODEV;
	if (ahci_devices[drive].type != AHCI_DEV_SATA) return -ENOSYS;

	uint8_t      port_idx = ahci_devices[drive].port;
	ahci_port_t *port     = &ahci_ports[port_idx];

	uint8_t *out  = (uint8_t *)buffer;
	uint8_t  left = numsects;

	while (left) {
		uint8_t  chunk = (left > AHCI_DMA_MAX_SECTORS) ? AHCI_DMA_MAX_SECTORS : left;
		uint32_t bytes = (uint32_t)chunk * 512;

		FIS_REG_H2D cfis;
		memset(&cfis, 0, sizeof(cfis));
		cfis.fis_type = FIS_TYPE_REG_H2D;
		cfis.c        = 1;
		cfis.command  = ATA_CMD_READ_DMA_EXT;
		cfis.device   = 1 << 6;

		cfis.lba0   = (uint8_t)(lba & 0xFF);
		cfis.lba1   = (uint8_t)((lba >> 8) & 0xFF);
		cfis.lba2   = (uint8_t)((lba >> 16) & 0xFF);
		cfis.lba3   = (uint8_t)((lba >> 24) & 0xFF);
		cfis.countl = chunk;

		int slot = ahci_port_find_slot(port);
		if (slot < 0) return -EBUSY;

		int ret = ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 0,
					 ahci_dma_buf_phys, bytes);
		if (ret != 0) return ret;

		memcpy(out, ahci_dma_buffer, bytes);
		out += bytes;
		lba += chunk;
		left -= chunk;
	}

	return 0;
}

int ahci_write_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, const void *buffer)
{
	if (drive >= AHCI_MAX_DEVICES || !ahci_devices[drive].reserved) return -ENODEV;
	if (ahci_devices[drive].type != AHCI_DEV_SATA) return -ENOSYS;

	uint8_t      port_idx = ahci_devices[drive].port;
	ahci_port_t *port     = &ahci_ports[port_idx];

	const uint8_t *in   = (const uint8_t *)buffer;
	uint8_t        left = numsects;

	while (left) {
		uint8_t  chunk = (left > AHCI_DMA_MAX_SECTORS) ? AHCI_DMA_MAX_SECTORS : left;
		uint32_t bytes = (uint32_t)chunk * 512;

		memcpy(ahci_dma_buffer, in, bytes);

		FIS_REG_H2D cfis;
		memset(&cfis, 0, sizeof(cfis));
		cfis.fis_type = FIS_TYPE_REG_H2D;
		cfis.c        = 1;
		cfis.command  = ATA_CMD_WRITE_DMA_EXT;
		cfis.device   = 1 << 6;

		cfis.lba0   = (uint8_t)(lba & 0xFF);
		cfis.lba1   = (uint8_t)((lba >> 8) & 0xFF);
		cfis.lba2   = (uint8_t)((lba >> 16) & 0xFF);
		cfis.lba3   = (uint8_t)((lba >> 24) & 0xFF);
		cfis.countl = chunk;

		int slot = ahci_port_find_slot(port);
		if (slot < 0) return -EBUSY;

		int ret = ahci_issue_cmd(port, slot, (uint8_t *)&cfis, 1,
					 ahci_dma_buf_phys, bytes);
		if (ret != 0) return ret;

		in += bytes;
		lba += chunk;
		left -= chunk;
	}

	return 0;
}

void init_ahci(void)
{
	pci_device_find(&ahci_pci_request);
	if (ahci_pci_request.response->error != PCI_FINDING_SUCCESS) {
		plogk("ahci: No AHCI controller found.\n");
		return;
	}

	pci_device_cache_t *cache = ahci_pci_request.response->device;

	base_address_register_t bar = get_base_address_register(cache, 5);
	if (bar.type != mem_mapping) {
		plogk("ahci: BAR5 is not a memory BAR.\n");
		return;
	}

	hba_mem = (volatile HBA_MEM *)bar.address;

	uint32_t cap2 = ahci_read_reg(&hba_mem->cap2);
	if (cap2 & (1 << 0)) {
		uint32_t bohc = ahci_read_reg(&hba_mem->bohc);
		if (bohc & (1 << 4)) {
			if (!(bohc & (1 << 1))) {
				ahci_write_reg(&hba_mem->bohc, bohc | (1 << 1));
				int boh_timeout = 1000000;
				while ((ahci_read_reg(&hba_mem->bohc) & (1 << 4)) && !(ahci_read_reg(&hba_mem->bohc) & (1 << 1))) {
					if (--boh_timeout <= 0) break;
				}
			}
		}
	}

	uint32_t ghc = ahci_read_reg(&hba_mem->ghc);
	ahci_write_reg(&hba_mem->ghc, ghc & ~(1u << 31));

	ahci_write_reg(&hba_mem->ghc, ahci_read_reg(&hba_mem->ghc) | (1u << 0));
	int reset_timeout = 1000000;
	while (ahci_read_reg(&hba_mem->ghc) & (1u << 0)) {
		if (--reset_timeout <= 0) {
			plogk("ahci: HBA reset timeout.\n");
			return;
		}
	}

	ahci_write_reg(&hba_mem->ghc, ahci_read_reg(&hba_mem->ghc) | (1u << 31));
	int ae_timeout = 1000000;
	while (!(ahci_read_reg(&hba_mem->ghc) & (1u << 31))) {
		if (--ae_timeout <= 0) break;
	}

	uint32_t pi = ahci_read_reg(&hba_mem->pi);
	uint32_t cap = ahci_read_reg(&hba_mem->cap);
	uint32_t max_ports = (cap & 0x1F) + 1;
	if (max_ports > AHCI_MAX_PORTS) max_ports = AHCI_MAX_PORTS;

	uint64_t dma_buf_phys = alloc_frames(AHCI_DMA_BUF_PAGES);
	ahci_dma_buf_phys      = dma_buf_phys;
	ahci_dma_buffer        = (uint8_t *)phys_to_virt(dma_buf_phys);

	ahci_port_count = 0;

	for (uint32_t i = 0; i < max_ports; i++) {
		if (!(pi & (1u << i))) continue;

		ahci_port_t *port = &ahci_ports[ahci_port_count];
		memset(port, 0, sizeof(*port));
		port->regs      = &hba_mem->ports[i];
		port->port_num  = (uint8_t)i;

		port->clb_phys  = alloc_frames(1);
		port->fb_phys   = alloc_frames(1);
		port->ct_phys   = alloc_frames(1);

		port->cmd_list = (HBA_CMD_HEADER *)phys_to_virt(port->clb_phys);
		port->fis      = (HBA_FIS *)phys_to_virt(port->fb_phys);
		port->cmd_tbl  = (HBA_CMD_TBL *)phys_to_virt(port->ct_phys);

		memset(port->cmd_list, 0, 0x400);
		memset((void *)port->fis, 0, 0x100);
		memset((void *)port->cmd_tbl, 0, 0x100);

		if (ahci_port_start(port) != 0) {
			plogk("ahci: Port %u start failed.\n", i);
			continue;
		}

		ahci_write_reg(&port->regs->sctl,
			(ahci_read_reg(&port->regs->sctl) & ~0xFu) | 0x1);
		{
			int det_timeout = 1000000;
			while (1) {
				uint32_t ssts = ahci_read_reg(&port->regs->ssts);
				if ((ssts & 0xF) == HBA_PORT_DET_PRESENT) break;
				if ((ssts & 0xF) == 0) break;
				if (--det_timeout <= 0) break;
			}
		}

		uint32_t sig = ahci_read_reg(&port->regs->sig);
		if (sig != SATA_SIG_ATA && sig != SATA_SIG_ATAPI) {
			plogk("ahci: Port %u no device (sig=0x%x).\n", i, sig);
			ahci_port_stop(port);
			continue;
		}

		ahci_device_t *dev = &ahci_devices[ahci_device_count];
		memset(dev, 0, sizeof(*dev));
		dev->port = (uint8_t)ahci_port_count;

		if (ahci_port_identify(port, dev) != 0) {
			plogk("ahci: Port %u identify failed.\n", i);
			ahci_port_stop(port);
			continue;
		}

		ahci_device_count++;
		ahci_port_count++;

		const char *type_str = (dev->type == AHCI_DEV_SATA) ? "ATA" : "ATAPI";
		plogk("ahci: Found %s Drive %u (KiB) on port %u - %s\n",
		      type_str, (dev->size * 512) / 1024, i, dev->model);
	}

	if (ahci_device_count == 0) {
		plogk("ahci: No AHCI devices found.\n");
	} else {
		plogk("ahci: %u device(s) initialized.\n", ahci_device_count);
	}
}
