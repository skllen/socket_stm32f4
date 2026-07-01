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
#ifndef __CMD_H__
#define __CMD_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/

/* USER CODE BEGIN Includes */
#include "main.h"
#include "uart_device.h"
#include "data_event.h"
/* USER CODE END Includes */


/* USER CODE BEGIN Private defines */
#define CMD_ARGV_MAX   10
#define CMD_LINE_MAX   256

struct Cmd_Device;

typedef struct Cmd_Device {
    const char *dev_name;      /* 既是名字也是匹配前缀: "cmd:led" */
    int  (*init)(struct Cmd_Device *self);
    int  (*Cmd_Callback)(struct Cmd_Device *self, int argc, char **argv);
    void *pri_data;
} Cmd_Device_t;

struct Cmd_Device *Get_Cmd(char *name);
static int Uart_Execut_Callback(struct inputEvent *ev);
int cmd_init(void);
void process_cmd_task(PUART_Device uart);
/* USER CODE BEGIN Prototypes */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */

