/* Includes ------------------------------------------------------------------*/
#include "usart.h"

/* USER CODE BEGIN 0 */
#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "uart_device.h"
#include "string.h"
#include "stdio.h"
/* USER CODE END 0 */


#define STM32_UART1_RX_QUEUE_LENGTH 512
#define STM32_UART1_RX_DMA_BUFFER   512
#define STM32_UART1_TX_UXITEMSIZE   1

#define STM32_UART2_RX_QUEUE_LENGTH 512
#define STM32_UART2_RX_DMA_BUFFER   512
#define STM32_UART2_TX_UXITEMSIZE   1

typedef struct UART_Data{
	SemaphoreHandle_t  xSemaphore_tx;
	QueueHandle_t 		 QueueHandle_rx_queue;
	UART_HandleTypeDef *huart;
	int r;
}* PUART_Data;

static struct UART_Data g_uart1_data ={
	.r		 = 0,
  .huart = &huart1
};

static struct UART_Data g_uart2_data ={
	.r		 = 0,
  .huart = &huart2
};

static int stm32_uart_init(struct UART_Device *pDev, int baud, int datas, char parity, int stop);
static int stm32_uart_send(struct UART_Device *pDev, uint8_t *data, int len, uint32_t timeout_ms);
static int stm32_uart_recv(struct UART_Device *pDev, uint8_t *data, uint32_t timeout_ms);
static int stm32_uart_flush(struct UART_Device *pDev);

static struct UART_Device uart1_stm32f4_hal = {
	.name         = "stm32_f4_uart1",
	.UART_Init    = stm32_uart_init,
	.UART_Send    = stm32_uart_send,
	.UART_Recv    = stm32_uart_recv,
	.UART_Flush		= stm32_uart_flush,
  .private_data   = &g_uart1_data
};

static struct UART_Device uart2_stm32f4_hal = {
	.name         = "stm32_f4_uart2",
	.UART_Init    = stm32_uart_init,
	.UART_Send    = stm32_uart_send,
	.UART_Recv    = stm32_uart_recv,
	.UART_Flush		= stm32_uart_flush,
  .private_data   = &g_uart2_data
};

static uint8_t g_uart1_recv_dma_buff[STM32_UART1_RX_DMA_BUFFER];
static uint8_t g_uart2_recv_dma_buff[STM32_UART2_RX_DMA_BUFFER];
static struct UART_Device *g_uart_dev_all[] = {&uart1_stm32f4_hal, &uart2_stm32f4_hal};

struct UART_Device *Get_UART_Device(char *name)
{
	int i;
    for(i =0 ;i < sizeof(g_uart_dev_all)/sizeof(struct UART_Device *); i++)
    {
        if(0 == strcmp(name,g_uart_dev_all[i]->name))
        {
            return g_uart_dev_all[i];
        }
    }
  return NULL;
}

static int stm32_uart_init(struct UART_Device *pDev, int baud, int datas, char parity, int stop)
{ 
  PUART_Data pdata = (PUART_Data )(pDev->private_data);

	pdata->xSemaphore_tx = xSemaphoreCreateBinary();
  
	pdata->QueueHandle_rx_queue = xQueueCreate( STM32_UART1_RX_QUEUE_LENGTH, STM32_UART1_TX_UXITEMSIZE);

  if(&huart1 ==  pdata->huart)
  {
	  HAL_UARTEx_ReceiveToIdle_DMA(pdata->huart, g_uart1_recv_dma_buff, sizeof(g_uart1_recv_dma_buff));
  }
  else if(&huart2 == pdata->huart)
  {
	HAL_UARTEx_ReceiveToIdle_DMA(pdata->huart, g_uart2_recv_dma_buff, sizeof(g_uart2_recv_dma_buff));
  }

  return 0;
} 
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  PUART_Data pdata;
//	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	if(g_uart1_data.huart == huart)
  {
    pdata = &g_uart1_data;
  }
  else if (g_uart2_data.huart == huart)
  {
		pdata =&g_uart2_data;
  }
//	xSemaphoreTakeFromISR(pdata->xSemaphore_tx, &xHigherPriorityTaskWoken);
	 xSemaphoreGiveFromISR(pdata->xSemaphore_tx, NULL);
}

static int stm32_uart_send(struct UART_Device *pDev, uint8_t *data, int len, uint32_t timeout_ms)
{
  PUART_Data pdata = (PUART_Data )pDev->private_data;
  HAL_UART_Transmit_DMA( pdata->huart, data, len);
	if (pdTRUE == xSemaphoreTake(pdata->xSemaphore_tx, timeout_ms))	
		return 0;
	else
		return -1;
}

static int stm32_uart_recv(struct UART_Device *pDev, uint8_t *data, uint32_t timeout_ms)
{
  PUART_Data pdata = (PUART_Data )pDev->private_data;
  BaseType_t err = xQueueReceive( pdata->QueueHandle_rx_queue,
                          (void *)data,
                          timeout_ms );
  return !err;
}

static int stm32_uart_flush(struct UART_Device *pDev)
{
	uint8_t data;
	  PUART_Data pdata = (PUART_Data )pDev->private_data;
  while(pdTRUE == xQueueReceive( pdata->QueueHandle_rx_queue,
                          &data,
                          0 ));
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	PUART_Data pdata;
	__HAL_UART_CLEAR_OREFLAG(huart);
	__HAL_UART_CLEAR_NEFLAG(huart);
	__HAL_UART_CLEAR_FEFLAG(huart);
	__HAL_UART_CLEAR_PEFLAG(huart);

	if (g_uart1_data.huart == huart)
	{
		pdata = &g_uart1_data;
		pdata->r = 0;
		HAL_UARTEx_ReceiveToIdle_DMA(huart, g_uart1_recv_dma_buff, sizeof(g_uart1_recv_dma_buff));
	}
	else if (g_uart2_data.huart == huart)
	{
		pdata = &g_uart2_data;
		pdata->r = 0;
		HAL_UARTEx_ReceiveToIdle_DMA(huart, g_uart2_recv_dma_buff, sizeof(g_uart2_recv_dma_buff));
	}
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
	PUART_Data pdata;
	if (g_uart1_data.huart == huart)
	{
		pdata = &g_uart1_data;
			for (int i = pdata->r; i < Size; i++)
	{
		xQueueSendFromISR(
			pdata->QueueHandle_rx_queue,
			&g_uart1_recv_dma_buff[i],
			0);
	}
	}
	else if (g_uart2_data.huart == huart)
	{
		pdata = &g_uart2_data;
			for (int i = pdata->r; i < Size; i++)
	{
		xQueueSendFromISR(
			pdata->QueueHandle_rx_queue,
			&g_uart2_recv_dma_buff[i],
			0);
	}
	}

	pdata->r = Size;
	if ((HAL_UART_RXEVENT_TC == huart->RxEventType))
	{
		pdata->r = 0;
		// HAL_UARTEx_ReceiveToIdle_DMA(huart, g_uart1_recv_dma_buff, sizeof(g_uart1_recv_dma_buff));
	}
	//	if((HAL_UART_RXEVENT_IDLE == huart->RxEventType) || (HAL_UART_RXEVENT_TC == huart->RxEventType))
	//	{
	//		pdata->r = 0;
	//		HAL_UARTEx_ReceiveToIdle_DMA(huart, g_uart1_recv_dma_buff, sizeof(g_uart1_recv_dma_buff));
	//	}
}

//int fputc(int ch, FILE *f)
//{
//	uart2_stm32f4_hal.UART_Send(&uart2_stm32f4_hal, (uint8_t *)&ch, 1, portMAX_DELAY);
//    return ch;
//}
// 
// int fgetc(FILE *f) 
// {
//    int ch;
//    //数据接收
//    uart2_stm32f4_hal.UART_Receive(&uart2_stm32f4_hal, (uint8_t *)&ch, 1, portMAX_DELAY);
//    return ch;
// }