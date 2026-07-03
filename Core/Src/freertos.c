/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "uart_device.h"
#include "mqttclient_test.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 5,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
extern void tcp_client_test_task(void *arg);

#define TCP_CLIENT_TASK_STACK_SIZE   512        /* 单位是字（word），不是字节 */
#define TCP_CLIENT_TASK_PRIORITY     (osPriorityNormal)

//static TaskHandle_t s_tcp_client_handle = NULL;

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
//	xTaskCreate(
//        tcp_client_test_task,          /* 任务函数 */
//        "tcp_client",                  /* 任务名，调试用 */
//        TCP_CLIENT_TASK_STACK_SIZE,    /* 栈深度，单位：字 */
//        NULL,                          /* 传给任务的参数 arg */
//        TCP_CLIENT_TASK_PRIORITY,      /* 优先级 */
//        &s_tcp_client_handle);         /* 任务句柄，不需要可传 NULL */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */

//  PUART_Device uart = Get_UART_Device("stm32_f4_uart2");
//  if (uart == NULL)
//  {
//    while(1)
//    {
//       osDelay(1);
//    }
//  }
//  uart->UART_Init(uart, 115200, 8, 'N', 1);
//  process_cmd_task();
		//esp8266_wifi_connect("Tenda", "wxw123456");
	MQTTClientTask(NULL);
  //   extern int esp8266_wifi_connect(char *ssid, char *password);
  // esp8266_wifi_connect("Tenda", "wxw123456");
  /* Infinite loop */
  for(;;)
  {
		HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_RESET);	
    vTaskDelay(500);
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_SET);	
    vTaskDelay(500);
//    if(0 == uart1->UART_Recv(uart1, &data, 1))
//    {
//       uart1->UART_Send(uart1, &data, 1, 1);
//    }
//		    if(0 == uart->UART_Recv(uart, &data, 1))
//    {
//       uart->UART_Send(uart, &data, 1, 1);
//    }
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

