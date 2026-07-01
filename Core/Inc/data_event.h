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
#ifndef __DATA_EVENT_H__
#define __DATA_EVENT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/

/* USER CODE BEGIN Includes */
#include "main.h"
/* USER CODE END Includes */
#define INPUT_BUFF_MAX_LEN  100

/* USER CODE BEGIN Private defines */
struct inputEvent{
    int type;//1-uart.2-key,3-touch
    char buf[INPUT_BUFF_MAX_LEN];//uart data
    int data_len;
    int code;//key -keybject ,
    int value;//1- 按下，0松开
};
struct outputEvent{
  int type;
  int (*Execut_Callback)(struct inputEvent *env);
  struct outputEvent *next;
  /* data */
};

void inputEvent_Init(void);
int inputEvent_Read(struct inputEvent *ev);

int inputEvent_Write(const struct inputEvent *ev);

int outputEvent_Execut(struct inputEvent *ev);

int outputEvent_register(struct outputEvent *ev);
/* USER CODE END Private defines */


/* USER CODE BEGIN Prototypes */

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */

