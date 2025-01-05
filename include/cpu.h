/*
 *
 *		cpu.h
 *		cpu相关操作头文件
 *
 *		2024/8/21 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_CPU_H_
#define INCLUDE_CPU_H_

// 来自各种CPU的厂商信息字符串
#define CPUID_VENDOR_AMD			"AuthenticAMD"
#define CPUID_VENDOR_AMD_OLD		"AMDisbetter!" // AMD K5早期工程样品的厂商信息
#define CPUID_VENDOR_INTEL			"GenuineIntel"
#define CPUID_VENDOR_VIA			"VIA VIA VIA "
#define CPUID_VENDOR_TRANSMETA		"GenuineTMx86"
#define CPUID_VENDOR_TRANSMETA_OLD	"TransmetaCPU"
#define CPUID_VENDOR_CYRIX			"CyrixInstead"
#define CPUID_VENDOR_CENTAUR		"CentaurHauls"
#define CPUID_VENDOR_NEXGEN			"NexGenDriven"
#define CPUID_VENDOR_UMC			"UMC UMC UMC "
#define CPUID_VENDOR_SIS			"SiS SiS SiS "
#define CPUID_VENDOR_NSC			"Geode by NSC"
#define CPUID_VENDOR_RISE			"RiseRiseRise"
#define CPUID_VENDOR_VORTEX			"Vortex86 SoC"
#define CPUID_VENDOR_AO486			"MiSTer AO486"
#define CPUID_VENDOR_AO486_OLD		"GenuineAO486"
#define CPUID_VENDOR_ZHAOXIN		"  Shanghai  "
#define CPUID_VENDOR_HYGON			"HygonGenuine"
#define CPUID_VENDOR_ELBRUS			"E2K MACHINE "
 
// 来自各种虚拟设备的厂商信息
#define CPUID_VENDOR_QEMU			"TCGTCGTCGTCG"
#define CPUID_VENDOR_KVM			" KVMKVMKVM  "
#define CPUID_VENDOR_VMWARE			"VMwareVMware"
#define CPUID_VENDOR_VIRTUALBOX		"VBoxVBoxVBox"
#define CPUID_VENDOR_XEN			"XenVMMXenVMM"
#define CPUID_VENDOR_HYPERV			"Microsoft Hv"
#define CPUID_VENDOR_PARALLELS		" prl hyperv "
#define CPUID_VENDOR_PARALLELS_ALT	" lrpepyh vr " // 有些并口会错误地把 "prl hyperv" 解码为 "lrpepyh vr" 由于字节序不匹配
#define CPUID_VENDOR_BHYVE			"bhyve bhyve "
#define CPUID_VENDOR_QNX			" QNXQVMBSQG "

enum {
	CPUID_FEAT_ECX_SSE3			= 1 << 0,
	CPUID_FEAT_ECX_PCLMUL		= 1 << 1,
	CPUID_FEAT_ECX_DTES64		= 1 << 2,
	CPUID_FEAT_ECX_MONITOR		= 1 << 3,
	CPUID_FEAT_ECX_DS_CPL		= 1 << 4,
	CPUID_FEAT_ECX_VMX			= 1 << 5,
	CPUID_FEAT_ECX_SMX			= 1 << 6,
	CPUID_FEAT_ECX_EST			= 1 << 7,
	CPUID_FEAT_ECX_TM2			= 1 << 8,
	CPUID_FEAT_ECX_SSSE3		= 1 << 9,
	CPUID_FEAT_ECX_CID			= 1 << 10,
	CPUID_FEAT_ECX_SDBG			= 1 << 11,
	CPUID_FEAT_ECX_FMA			= 1 << 12,
	CPUID_FEAT_ECX_CX16			= 1 << 13,
	CPUID_FEAT_ECX_XTPR			= 1 << 14,
	CPUID_FEAT_ECX_PDCM			= 1 << 15,
	CPUID_FEAT_ECX_PCID			= 1 << 17,
	CPUID_FEAT_ECX_DCA			= 1 << 18,
	CPUID_FEAT_ECX_SSE4_1		= 1 << 19,
	CPUID_FEAT_ECX_SSE4_2		= 1 << 20,
	CPUID_FEAT_ECX_X2APIC		= 1 << 21,
	CPUID_FEAT_ECX_MOVBE		= 1 << 22,
	CPUID_FEAT_ECX_POPCNT		= 1 << 23,
	CPUID_FEAT_ECX_TSC			= 1 << 24,
	CPUID_FEAT_ECX_AES			= 1 << 25,
	CPUID_FEAT_ECX_XSAVE		= 1 << 26,
	CPUID_FEAT_ECX_OSXSAVE		= 1 << 27,
	CPUID_FEAT_ECX_AVX			= 1 << 28,
	CPUID_FEAT_ECX_F16C			= 1 << 29,
	CPUID_FEAT_ECX_RDRAND		= 1 << 30,
	CPUID_FEAT_ECX_HYPERVISOR	= 1 << 31,

	CPUID_FEAT_EDX_FPU			= 1 << 0,
	CPUID_FEAT_EDX_VME			= 1 << 1,
	CPUID_FEAT_EDX_DE			= 1 << 2,
	CPUID_FEAT_EDX_PSE			= 1 << 3,
	CPUID_FEAT_EDX_TSC			= 1 << 4,
	CPUID_FEAT_EDX_MSR			= 1 << 5,
	CPUID_FEAT_EDX_PAE			= 1 << 6,
	CPUID_FEAT_EDX_MCE			= 1 << 7,
	CPUID_FEAT_EDX_CX8			= 1 << 8,
	CPUID_FEAT_EDX_APIC			= 1 << 9,
	CPUID_FEAT_EDX_SEP			= 1 << 11,
	CPUID_FEAT_EDX_MTRR			= 1 << 12,
	CPUID_FEAT_EDX_PGE			= 1 << 13,
	CPUID_FEAT_EDX_MCA			= 1 << 14,
	CPUID_FEAT_EDX_CMOV			= 1 << 15,
	CPUID_FEAT_EDX_PAT			= 1 << 16,
	CPUID_FEAT_EDX_PSE36		= 1 << 17,
	CPUID_FEAT_EDX_PSN			= 1 << 18,
	CPUID_FEAT_EDX_CLFLUSH		= 1 << 19,
	CPUID_FEAT_EDX_DS			= 1 << 21,
	CPUID_FEAT_EDX_ACPI			= 1 << 22,
	CPUID_FEAT_EDX_MMX			= 1 << 23,
	CPUID_FEAT_EDX_FXSR			= 1 << 24,
	CPUID_FEAT_EDX_SSE			= 1 << 25,
	CPUID_FEAT_EDX_SSE2			= 1 << 26,
	CPUID_FEAT_EDX_SS			= 1 << 27,
	CPUID_FEAT_EDX_HTT			= 1 << 28,
	CPUID_FEAT_EDX_TM			= 1 << 29,
	CPUID_FEAT_EDX_IA64			= 1 << 30,
	CPUID_FEAT_EDX_PBE			= 1 << 31
};

enum cpuid_requests {
	CPUID_GETVENDORSTRING,
	CPUID_GETFEATURES,
	CPUID_GETTLB,
	CPUID_GETSERIAL,

	CPUID_INTELEXTENDED = 0x80000000,
	CPUID_INTELFEATURES,
	CPUID_INTELBRANDSTRING,
	CPUID_INTELBRANDSTRINGMORE,
	CPUID_INTELBRANDSTRINGEND,
};

typedef struct {
	char* vendor;
	char model_name[64];
	unsigned int virt_bits;
	unsigned int phys_bits;
} cpu_t;

/* 获取CPUID */
void cpuid(unsigned int op, unsigned int *eax, unsigned int *ebx, unsigned int *ecx, unsigned int *edx);

/* 获取CPU厂商名称 */
void get_vendor_name(cpu_t *c);

/* 获取CPU型号名称 */
void get_model_name(cpu_t *c);

/* 获取CPU地址大小 */
void get_cpu_address_sizes(cpu_t *c);

/* 打印CPU信息 */
void print_cpu_info(void);

/* 获取CPU信息 */
void get_cpu_info(char **VENDOR, char **MODEL_NAME, int *PHYS_BITS, int *VIRT_BITS);

#endif // INCLUDE_CPU_H_
