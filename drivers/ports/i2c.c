/*
 *
 *		i2c.c
 *		I2C总线通信协议
 *
 *		2024/12/1 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "i2c.h"

/* 生成I2C起始信号 */
void IIC_GenerateStart(void)
{
	IIC_SDA_OUT();
	IIC_SDA_OUT_HIGH();
	IIC_SCL_OUT_HIGH();
	IIC_Delay();
	IIC_SDA_OUT_LOW();
	IIC_Delay();
	IIC_SCL_OUT_LOW();
	IIC_Delay();
}

/* 生成I2C停止信号 */
void IIC_GenerateStop(void)
{
	IIC_SDA_OUT();
	IIC_SDA_OUT_LOW();
	IIC_SCL_OUT_HIGH();
	IIC_Delay();
	IIC_SDA_OUT_HIGH();
	IIC_Delay();
}

/* 发送一个应答信号 */
void IIC_Acknowledge(void)
{
	IIC_SDA_OUT();
	IIC_SDA_OUT_LOW();
	IIC_Delay();
	IIC_SCL_OUT_HIGH();
	IIC_Delay();
	IIC_SCL_OUT_LOW();
	IIC_Delay();
	IIC_SDA_OUT_HIGH();
}

/* 发送一个非应答信号 */
void IIC_NoAcknowledge(void)
{
	IIC_SDA_OUT();
	IIC_SDA_OUT_LOW();
	IIC_SCL_OUT_HIGH();
	IIC_Delay();
	IIC_SDA_OUT_HIGH();
}

/* 接收来自从设备的应答信号 */
int IIC_ReceiveAcknowledge(void)
{
	uint8_t temp;
	IIC_SDA_OUT();
	IIC_SDA_OUT_HIGH();
	IIC_Delay();
	IIC_SCL_OUT_HIGH();
	IIC_Delay();
	temp = IIC_SDA_IN();
	IIC_SCL_OUT_LOW();
	IIC_Delay();
	if (temp != 0) {
		return 1;
	} else {
		return 0;
	}
}

/* 发送一个字节数据 */
void IIC_SendByte(uint8_t byte)
{
	uint8_t i;
	for ( i = 0; i < 8; i++) {
		if (byte & 0x80) {
			IIC_SDA_OUT_HIGH();
		} else {
			IIC_SDA_OUT_LOW();
		}
		IIC_Delay();
		IIC_SCL_OUT_HIGH();
		IIC_Delay();
		IIC_SCL_OUT_LOW();
		if (i == 7) {
			IIC_SDA_OUT_HIGH();
		}
		byte <<= 1;
		IIC_Delay();
	};
};

/* 接收一个字节数据 */
int32_t IIC_ReceiveByte(void)
{
	uint8_t i;
	uint8_t temp = 0;
	for(i = 0; i < 8; i++) {
		temp <<= 1;
		IIC_SCL_OUT_HIGH();
		IIC_Delay();
		if(IIC_SDA_IN()) {
			temp++;
		}
		IIC_SCL_OUT_LOW();
		IIC_Delay();
	}
	return temp;
};

/* 发送写操作的设备地址 */
int IIC_SendWriteAddress(iic_client *client)
{
	IIC_GenerateStart();
	IIC_SendByte(client->address & 0x01);
	return IIC_ReceiveAcknowledge();
}

/* 发送读操作的设备地址 */
int IIC_SendReadAddress(iic_client *client)
{
	IIC_GenerateStart();
	IIC_SendByte(client->address | 0x00);
	return IIC_ReceiveAcknowledge();
}

/* 检查I2C设备是否响应 */
int IIC_CheckClient(iic_client *client)
{
	IIC_GenerateStart();
	IIC_SendByte(client->address | 0x01);
	IIC_GenerateStop();
	return IIC_ReceiveAcknowledge();
};
