/*
 *
 *		acpi.c
 *		高级配置与电源接口驱动
 *
 *		2024/11/2 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "acpi.h"
#include "boot.h"
#include "printk.h"
#include "common.h"
#include "timer.h"
#include "memory.h"

uint16_t SLP_TYPa;
uint16_t SLP_TYPb;
uint32_t SMI_CMD;
uint8_t ACPI_ENABLE;
uint8_t ACPI_DISABLE;
uint32_t *PM1a_CNT;
uint32_t *PM1b_CNT;
uint8_t PM1_CNT_LEN;
uint16_t SLP_EN;
uint16_t SCI_EN;
uint32_t PM1a_EVT_BLK;
uint32_t PM1b_EVT_BLK;

void *acpi_base = NULL;
uintptr_t acpi_start = 0;
uintptr_t acpi_end = 0;
acpi_rsdt_t *rsdt = NULL; // root system descript table
acpi_facp_t *facp = NULL; // fixed ACPI table

int acpi_enable_flag;
uint8_t *rsdp_address;

HpetInfo *hpetInfo = 0;
uint32_t hpetPeriod = 0;

/* 初始化高级配置与电源接口（ACPI） */
void acpi_init(void)
{
	print_busy("Initializing Advanced Configuration and Power Interface...\r"); // 提示用户正在初始化ACPI，并回到行首等待覆盖
	acpi_sys_init();
	acpi_enable_flag = !acpi_enable();
	hpet_initialize();
	acpi_power_init();
	print_succ("The Advanced Configuration and Power Interface initialization is successful.\n");
}

static void *acpi_locate(uintptr_t addr) {
  if ((addr >= acpi_start) && (addr < acpi_end))
    return (void *)(((uintptr_t)acpi_base) + (addr - acpi_start));
  return NULL;
}

/* 系统ACPI初始化 */
int acpi_sys_init(void)
{
	acpi_start = boot_info.acpi.start;
	acpi_end = boot_info.acpi.end;
	if (acpi_start < acpi_end)
		acpi_base = page_map_kernel_range(acpi_start, acpi_end, 0);

	rsdt = (acpi_rsdt_t *)acpi_locate(boot_info.rsdt);
	if (acpi_check_header(rsdt, "RSDT") < 0) {
		print_warn("Unable to find Advanced Configuration and Power Interface.\n");
		return 0;
	}

	facp = (acpi_facp_t *)acpi_find_table("FACP");
	if (facp == NULL) {
		print_warn("Advanced Configuration and Power Interface: There is no valid FACP\n");
		return -1;
	}

	ACPI_ENABLE = facp->ACPI_ENABLE;
	ACPI_DISABLE = facp->ACPI_DISABLE;

	SMI_CMD = facp->SMI_CMD;

	PM1a_CNT = (uint32_t *)facp->PM1a_CNT_BLK;
	PM1b_CNT = (uint32_t *)facp->PM1b_CNT_BLK;

	PM1_CNT_LEN = facp->PM1_CNT_LEN;

	PM1a_EVT_BLK = facp->PM1b_EVT_BLK;
	PM1b_EVT_BLK = facp->PM1b_EVT_BLK;

	SLP_EN = 1 << 13;
	SCI_EN = 1;

	acpi_dsdt_t *dsdt = (acpi_dsdt_t *)acpi_locate(facp->DSDT);
	if (acpi_check_header(dsdt, "DSDT") < 0) {
		print_warn("Advanced Configuration and Power Interface: No DSDT Table found!\n");
	} else {
		uint32_t dsdtlen = (dsdt->length - sizeof(*dsdt))/sizeof(dsdt->definition_block[0]);
		uint8_t *S5Addr = dsdt->definition_block;
		while (dsdtlen--) {
			if (!memcmp(S5Addr, "_S5_", 4)) {
				break;
			}
			S5Addr++;
		}
		if (dsdtlen) {
			if (*(S5Addr - 1) == 0x08 || (*(S5Addr - 2) == 0x08 && *(S5Addr - 1) == '\\')) {
				S5Addr += 5;
				S5Addr += ((*S5Addr & 0xC0) >> 6) + 2;
				if (*S5Addr == 0x0A) {
					S5Addr++;
				}
				SLP_TYPa = *(S5Addr) << 10;
				S5Addr++;
				if (*S5Addr == 0x0A) {
					S5Addr++;
				}
				SLP_TYPb = *(S5Addr) << 10;
				S5Addr++;
			} else {
				print_warn("Advanced Configuration and Power Interface: _S5 Parse error!\n");
			}
		} else {
			print_warn("Advanced Configuration and Power Interface: _S5 Not present!\n");
		}
	}
	return 1;
}

/* 启用ACPI */
int acpi_enable(void)
{
	int i;
	if (inw((uint32_t) PM1a_CNT) & SCI_EN) {
		return 0;
	}
	if (SMI_CMD && ACPI_ENABLE) {
		outb((uint16_t)
		SMI_CMD, ACPI_ENABLE);

		for (i = 0; i < 300; i++) {
			if (inw((uint32_t) PM1a_CNT) & SCI_EN) {
				break;
			}
			clock_sleep(5);
		}
		if (PM1b_CNT) {
			for (i = 0; i < 300; i++) {
				if (inw((uint32_t) PM1b_CNT) & SCI_EN) {
					break;
				}
				clock_sleep(5);
			}
		}
		if (i < 300) {
			return 0;
		} else {
			return -1;
		}
	}
	return -1;
}

/* 禁用ACPI */
int acpi_disable(void)
{
	int i;
	if ((!inw((uint16_t)(uintptr_t)PM1a_CNT)) & SCI_EN)
	return 0;
	if (SMI_CMD || ACPI_DISABLE) {
		outb((uint16_t)
		SMI_CMD, ACPI_DISABLE);
		for (i = 0; i < 300; i++) {
			if ((!inw((uint16_t)(uintptr_t)PM1a_CNT)) & SCI_EN)
			break;
			clock_sleep(5);
		}
		for (i = 0; i < 300; i++) {
			if ((!inw((uint16_t)(uintptr_t)PM1b_CNT)) & SCI_EN)
			break;
			clock_sleep(5);
		}
		if (i < 300) {
			return 0;
		} else {
			return -1;
		}
	}
	return -1;
}

/* 初始化高精度事件计时器（HPET） */
void hpet_initialize(void)
{
	HPET *hpet = (HPET *)acpi_find_table("HPET");
	/* lower 32-bit */
	uint32_t addr = hpet->hpetAddress.address & 0xFFFFFFFFUL;
	/* consumes 1K of system memory, regardless of how many
	 * comparators are actually implemented by the hardware */
	hpetInfo = (HpetInfo *)page_map_kernel_range(addr, addr+1024, PT_W);
	uint32_t counterClockPeriod = hpetInfo->generalCapabilities >> 32;
	hpetPeriod = counterClockPeriod / 1000000;
	hpetInfo->generalConfiguration |= 1;
}

/* 初始化ACPI电源管理 */
void acpi_power_init(void)
{
	if (!facp) return;
	uint8_t len = facp->PM1_EVT_LEN / 2;
	uint32_t *PM1a_ENABLE_REG = (uint32_t *)facp->PM1a_EVT_BLK + len;
	uint32_t *PM1b_ENABLE_REG = (uint32_t *)facp->PM1b_EVT_BLK + len;
	if ((size_t)PM1b_ENABLE_REG == (size_t)len)
		PM1b_ENABLE_REG = 0;
	outw((uint16_t)(uintptr_t)PM1a_ENABLE_REG, (1 << 8));
	if (PM1b_ENABLE_REG) {
		outw((uint16_t)(uintptr_t)PM1b_ENABLE_REG, (uint8_t)(1 << 8));
	}
	register_interrupt_handler(facp->SCI_INT + 0x20, acpi_power_handler);
}

/* ACPI电源管理中断处理程序 */
void acpi_power_handler(struct interrupt_frame *frame)
{
	uint16_t status = inw((uint32_t) PM1a_EVT_BLK);

	outw((uint32_t) PM1a_EVT_BLK, status &= ~(1 << 8));
	power_off(); // By rainy101112 2024-11-02 注意这边现在其实不完善，以后需要加上把所有进程杀掉

	if (status & (1 << 8)) {
		return;
	}
	if (!PM1b_EVT_BLK){
		return;
	}
	status = inw((uint32_t) PM1b_EVT_BLK);
	if (status & (1 << 8)) {
		outw((uint32_t) PM1b_EVT_BLK, status &= ~(1 << 8));
		power_off(); // By rainy101112 2024-11-02 注意这边现在其实不完善，以后需要加上把所有进程杀掉
		return;
	}
}

/* 关闭电源 */
void power_off(void)
{
	if (!SCI_EN)
		return;
	while (1) {
		outw((uint32_t) PM1a_CNT, SLP_TYPa | SLP_EN);
		if (!PM1b_CNT) {
			outw((uint32_t) PM1b_CNT, SLP_TYPb | SLP_EN);
		}
	}
}

/* 重启系统 */
void power_reset(void)
{
	if (!SCI_EN)
		return;
	while (1) {
		outb(0x92, 0x01);
		outb((uint32_t) facp->RESET_REG.address, facp->RESET_VALUE);
	}
}

/* 检查ACPI表头 */
int acpi_check_header(void *ptr, const char *sign)
{
	if (ptr == NULL)
		return -1;
	uint8_t *bptr = ptr;
	uint32_t len = *(bptr + 4);
	uint8_t checksum = 0;
	if (!memcmp(bptr, sign, 4)) {
		while (len-- > 0) {
			checksum += *bptr++;
		}
		return 0;
	}
	return -1;
}

/* 查找ACPI表 */
void *acpi_find_table(const char *Signature)
{
	size_t n = (rsdt->length - sizeof(*rsdt))/sizeof(rsdt->entry[0]);
	for (size_t i=0; i<n; ++i) {
		void *header = acpi_locate(rsdt->entry[i]);
		if (acpi_check_header(header, Signature) < 0)
			continue;
		return header;
	}
	return NULL;
}

/* 获取多处理器系统的中断控制器表基地址 */
void *AcpiGetMadtBase(void)
{
	return acpi_find_table("APIC");
}

/* 获取纳秒级时间 */
uint32_t nano_time(void)
{
	if(hpetInfo == 0) return 0;
	uint32_t mcv =  hpetInfo->mainCounterValue;
	return mcv * hpetPeriod;
}

/* 微秒级睡眠 */
void usleep(uint32_t nano)
{
	uint32_t targetTime = nano_time();
	uint32_t after = 0;
	while (1) {
		uint64_t n = nano_time();
		if (n < targetTime) {
			after += 0xffffffff - targetTime + n;
			targetTime = n;
		} else {
			after += n - targetTime;
			targetTime = n;
		}
		if (after >= nano) {
			return;
		}
	}
}
