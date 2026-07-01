/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.h
  * @brief   This file contains all the function prototypes for
  *          the usart.c file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USART_DEVICE_H__
#define __USART_DEVICE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

typedef struct UART_Device{
	char *name;
	int (*UART_Init)(struct UART_Device *pDev, int baud, int datas, char parity, int stop);
	int (*UART_Send)(struct UART_Device *pDev, uint8_t *data, int len, uint32_t timeout_ms);
	int (*UART_Recv)(struct UART_Device *pDev, uint8_t *data, uint32_t timeout_ms);
	int (*UART_Flush)(struct UART_Device *pDev);
	void * private_data;
}* PUART_Device; 

struct UART_Device * Get_UART_Device(char *name);
/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */



/* USER CODE BEGIN Prototypes */
struct UART_Device * Get_UART_Device(char *name);
/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */

