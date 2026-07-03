// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (c) 2008-2023 100askTeam : Dongshan WEI <weidongshan@qq.com> 
 * Discourse:  https://forums.100ask.net
 */
 
/*  Copyright (C) 2008-2023 深圳百问网科技有限公司
 *  All rights reserved
 *
 * 免责声明: 百问网编写的文档, 仅供学员学习使用, 可以转发或引用(请保留作者信息),禁止用于商业用途！
 * 免责声明: 百问网编写的程序, 可以用于商业用途, 但百问网不承担任何后果！
 * 
 * 本程序遵循GPL V3协议, 请遵循协议
 * 百问网学习平台   : https://www.100ask.net
 * 百问网交流社区   : https://forums.100ask.net
 * 百问网官方B站    : https://space.bilibili.com/275908810
 * 本程序所用开发板 : STM32H5
 * 百问网官方淘宝   : https://100ask.taobao.com
 * 联系我们(E-mail): weidongshan@qq.com
 *
 *          版权所有，盗版必究。
 *  
 * 修改历史     版本号           作者        修改内容
 *-----------------------------------------------------
 * 2024.06.23      v01         百问科技      创建文件
 *-----------------------------------------------------
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/

#include <string.h>
#include "device_socket.h"
#include "MQTTClient.h"

#define PC_MQTT_BROKER_IP "192.168.0.152"
#define PC_MQTT_BROKER_PORT 1883

static void messageArrived(MessageData* md)
{
	static int cnt = 0;
	MQTTMessage* m = md->message;
	char buf[256];

	snprintf(buf, sizeof(buf), "get msg %d: %s", cnt++, (char *)m->payload);
//	buf[256] = '\0';
	printf("sub %s\r\n",buf);
	//Draw_String(0, 200, buf, 0xff0000, 0);
}

static void test1(void)
{
	int subsqos = 2;
	Network n;
	MQTTClient c;
	int rc = 0;
	char* sub_topic = "/topic/humiture";
	char* pub_topic = "/topic/temp";
	MQTTPacket_willOptions wopts;
	unsigned char buf[256];
	unsigned char readbuf[256];
	char pubbuf[256];
	int cnt = 0;
	int wait_seconds;

	MQTTMessage pubmsg;

	NetworkInit(&n);

	while (0 != NetworkConnect(&n, PC_MQTT_BROKER_IP, PC_MQTT_BROKER_PORT))
	{
		printf("Re-Connect TCP/Port ...\r\n");
		//Draw_String(0, 64, "Re-Connect TCP/Port ...", 0xff0000, 0);
		vTaskDelay(100);
	}
  
	MQTTClientInit(&c, &n, 1000, buf,  sizeof(buf), readbuf,  sizeof(readbuf));

	MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
	data.willFlag = 1;
	data.clientID.cstring = "100ask_mqtt_test";
	data.username.cstring = "testuser";
	data.password.cstring = "testpassword";

	data.keepAliveInterval = 20;
	data.cleansession = 1;

	data.will.message.cstring = "will message";
	data.will.qos = 1;
	data.will.retained = 0;
	data.will.topicName.cstring = "will topic";
	printf("Connect MQTT Broker ...\r\n");
	//Draw_String(0, 80, "Connect MQTT Broker ...", 0xff0000, 0);
	while (SUCCESS != MQTTConnect(&c, &data))
	{
		printf("Re-Connect MQTT Broker ...\r\n");
		vTaskDelay(1000);
		//Draw_String(0, 96, "Re-Connect MQTT Broker ...", 0xff0000, 0);
	}
	printf("MQTTSubscribe ...\r\n");
	//Draw_String(0, 96, "MQTTSubscribe ...", 0xff0000, 0);
	rc = MQTTSubscribe(&c, sub_topic, subsqos, messageArrived);

	while (1)
	{
		memset(&pubmsg, '\0', sizeof(pubmsg));
		sprintf(pubbuf, "msg from H5, %d", cnt++);
		pubmsg.payload = pubbuf;
		pubmsg.payloadlen = strlen(pubbuf);
		pubmsg.qos = 0;
		pubmsg.retained = 0;
		pubmsg.dup = 0;
	printf("isconnected=%d\r\n", c.isconnected); 
		//Draw_String(0, 112, pubbuf, 0xff0000, 0);
		rc = MQTTPublish(&c, pub_topic, &pubmsg);
		if(rc != 0)
		{
			printf("publish err %d\r\n",rc);
		}
		  /* wait for the message to be received */
		wait_seconds = 10;
		while (wait_seconds-- > 0)
		{
			MQTTYield(&c, 100);
		}
		
	}

}


/**********************************************************************
 * 函数名称： MQTTClientTask
 * 功能描述： MQTTClient任务
 * 输入参数： pvParameters - 未使用
 * 输出参数： 无
 * 返 回 值： 无
 * 修改日期：      版本号     修改人       修改内容
 * -----------------------------------------------
 * 2024/06/23        V1.0     韦东山       创建
 ***********************************************************************/
void MQTTClientTask( void *pvParameters )	
{
    int err;


	at_init("stm32_f4_uart2");

	while (1)
	{
		err = at_connect_ap("Tenda", "wxw123456");
		if (!err)
			break;
		else
		{
			vTaskDelay(1000);
		}
	}

	//Draw_String(0, 48, "Connect TCP/Port ...", 0xff0000, 0);

	while (1)
	{
		test1();
	}

    vTaskDelete(NULL);
}


