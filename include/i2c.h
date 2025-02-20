/*
 *
 *		i2c.h
 *		I2C总线通信协议头文件
 *
 *		2024/12/1 By Vinbe Wan
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#ifndef INCLUDE_I2C_H_
#define INCLUDE_I2C_H_

#include "types.h"
#include "common.h"

#define I2C_NAME_SIZE	32
#define STATIC_INLINE	static inline
#define GPIO_Port		0x12345678
#define IIC_Pin			0x01

STATIC_INLINE void IIC_SDA_OUT(void)
{
	outl(
		(uint16_t)GPIO_Port, (inl((uint16_t)GPIO_Port) | (0x01 << IIC_Pin))
	);
}

STATIC_INLINE void IIC_SDA_OUT_HIGH(void)
{
	outl(
		(uint16_t)GPIO_Port, (inl((uint16_t)GPIO_Port) | (0x01 << IIC_Pin))
	);
}

STATIC_INLINE void IIC_SDA_OUT_LOW(void)
{
	outl(
		(uint16_t)GPIO_Port, (inl((uint16_t)GPIO_Port) | (0x00 << IIC_Pin))
	);
}

STATIC_INLINE uint32_t IIC_SDA_IN(void)
{
	return inl((uint16_t)GPIO_Port);
}

STATIC_INLINE void IIC_SCL_OUT_HIGH(void )
{
	outl(
		(uint16_t)GPIO_Port, (inl((uint16_t)GPIO_Port) | (0x01 << IIC_Pin))
	);
}

STATIC_INLINE void IIC_SCL_OUT_LOW(void)
{
	outl(
		(uint16_t)GPIO_Port, (inl((uint16_t)GPIO_Port) | (0x00 << IIC_Pin))
	);
}

STATIC_INLINE void IIC_Delay(void)
{
	volatile uint8_t i;
	/*
		*AT32F425F6P7,
		*i = 100,SCL = 163.4KHZ,6.1us
		*i = 75, SCL = 243.9KHZ,4.1us
		*i = 50, SCL = 312.5kHZ,3.2us
	*/
	for(i=0;i<75;i++);
}

typedef struct {
	char name[I2C_NAME_SIZE];
} iic_device_id;

typedef struct {
	uint8_t address;
	iic_device_id i2CDeviceId;
} iic_client;

typedef struct {
	uint16_t length;
	uint8_t *buffer;
} iic_message;

typedef struct {
	int (*probe)(iic_client *, const iic_device_id *);
	int (*remove)(iic_client *);
} iic_driver;

/* 生成I2C起始信号 */
void IIC_GenerateStart(void);

/* 生成I2C停止信号 */
void IIC_GenerateStop(void);

/* 发送一个应答信号 */
void IIC_Acknowledge(void);

/* 发送一个非应答信号 */
void IIC_NoAcknowledge(void);

/* 接收来自从设备的应答信号 */
int IIC_ReceiveAcknowledge(void);

/* 发送一个字节数据 */
void IIC_SendByte(uint8_t byte);

/* 接收一个字节数据 */
int32_t IIC_ReceiveByte(void);

/* 发送写操作的设备地址 */
int IIC_SendWriteAddress(iic_client *client);

/* 发送读操作的设备地址 */
int IIC_SendReadAddress(iic_client *client);

/* 检查I2C设备是否响应 */
int IIC_CheckClient(iic_client *client);

#endif // INCLUDE_I2C_H_
