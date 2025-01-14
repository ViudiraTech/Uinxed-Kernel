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

acpi_rsdt_t *rsdt; // root system descript table
acpi_facp_t *facp; // fixed ACPI table

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

/* 系统ACPI初始化 */
int acpi_sys_init(void)
{
	uint32_t **p;
	uint32_t entrys;

	rsdt = (acpi_rsdt_t *) AcpiGetRSDPtr();
	page_line(rsdt);
	if (!rsdt || acpi_check_header(rsdt, (uint8_t*)"RSDT") < 0) {
		print_warn("Unable to find Advanced Configuration and Power Interface.\n");
		return 0;
	}
	entrys = rsdt->length - HEADER_SIZE / 4;
	p = &(rsdt->entry);
	while (entrys--) {
		page_line(*p);
		if (!acpi_check_header(*p, (uint8_t*)"FACP")) {
			facp = (acpi_facp_t *) *p;

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

			uint8_t * S5Addr;
			uint32_t dsdtlen;
			page_line(facp->DSDT);

			if (!acpi_check_header(facp->DSDT, (uint8_t*)"DSDT")) {
				S5Addr = &(facp->DSDT->definition_block);
				dsdtlen = facp->DSDT->length - HEADER_SIZE;
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
			} else {
				print_warn("Advanced Configuration and Power Interface: No DSDT Table found!\n");
			}
			return 1;
		}
		++p;
	}
	print_warn("Advanced Configuration and Power Interface: There is no valid FACP\n");
	return -1;
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
	HPET *hpet = (HPET *)(unsigned long)acpi_find_table("HPET");
	hpetInfo = (HpetInfo *) hpet->hpetAddress.address;
	page_line(hpetInfo);
	uint32_t counterClockPeriod = hpetInfo->generalCapabilities >> 32;
	hpetPeriod = counterClockPeriod / 1000000;
	hpetInfo->generalConfiguration |= 1;
}

/* ACPI电源管理中断处理程序 */
static void acpi_power_handler(struct interrupt_frame *frame)
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
	register_interrupt_handler(facp->SCI_INT + 0x20, &acpi_power_handler);
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

/* 获取ACPI RSD PTR地址 */
uint8_t *AcpiGetRSDPtr(void)
{
	uint32_t *addr;
	uint32_t *rsdt;
	uint32_t ebda;

	for (addr = (uint32_t *) 0x000E0000; addr < (uint32_t *) 0x00100000; addr += 0x10 / sizeof(addr)) {
         rsdt = (uint32_t *)AcpiCheckRSDPtr(addr);
		if (rsdt) {
			return (uint8_t *)rsdt;
		}
	}

	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Warray-bounds"
	ebda = *(uint16_t *) 0x40E;
	#pragma GCC diagnostic pop

	ebda = ebda * 0x10 & 0xfffff;
	for (addr = (uint32_t *)ebda; addr < (uint32_t * )(ebda + 1024); addr += 0x10 / sizeof(addr)) {
		rsdt = (uint32_t *)AcpiCheckRSDPtr(addr);
		if (rsdt) {
			return (uint8_t *)rsdt;
		}
	}
	return 0;
}

/* 检查ACPI表头 */
int acpi_check_header(void *ptr, uint8_t *sign)
{
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
unsigned int acpi_find_table(const char *Signature)
{
	uint8_t * ptr, *ptr2;
	uint32_t len;
	uint8_t * rsdt_t = (uint8_t *)rsdt;
	for (len = *((uint32_t *)(rsdt_t + 4)), ptr2 = rsdt_t + 36; ptr2 < rsdt_t + len;
		ptr2 += (*(uint8_t *)rsdt_t == 'X') ? 8 : 4) {
		ptr = (uint8_t * )(uintptr_t)(rsdt_t[0] == 'X' ? *((uint64_t *) ptr2)
              : *((uint32_t *) ptr2));
		if (!memcmp(ptr, Signature, 4)) {
			return (unsigned) ptr;
		}
	}
	return 0;
}

/* 检查RSD PTR */
uint8_t *AcpiCheckRSDPtr(void *ptr)
{
	const char *sign = "RSD PTR ";
	acpi_rsdptr_t *rsdp = ptr;
	uint8_t * bptr = ptr;
	uint32_t i = 0;
	uint8_t check = 0;
	if (!memcmp(sign, bptr, 8)) {
		rsdp_address = bptr;
		for (i = 0; i < sizeof(acpi_rsdptr_t); i++) {
			check += *bptr;
			bptr++;
		}
		if (!check) {
			return (uint8_t * ) rsdp->rsdt;
		}
	}
	return 0;
}

/* 获取多处理器系统的中断控制器表基地址 */
uint32_t AcpiGetMadtBase(void)
{
	uint32_t entrys = rsdt->length - HEADER_SIZE / 4;
	uint32_t **p = &(rsdt->entry);
	acpi_madt_t *madt = 0;
	while (--entrys) {
		if (!acpi_check_header(*p, (uint8_t*)"APIC")) {
			madt = (acpi_madt_t *) *p;
			return (uint32_t)madt;
		}
		p++;
	}
	return 0;
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
