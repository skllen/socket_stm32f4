#include "at_device.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include <string.h> 

void at_reset_resp(struct AT_Device * ptDev)
{
    memset(ptDev->resp_line_len,0,RESP_ROW_LEN);
    //
    ptDev->resp_line_counts = 0;
    ptDev->resp_status = 0;
}

/**
 * @brief  发送 AT 数据
 * @param  data 传输的数据
 * @param  len 数据长度
 * @param  timeout_ms 超时时间（毫秒）
 * @return 0-成功  其他-失败
 */
int at_send_data(struct AT_Device *pdev,const uint8_t *data, uint32_t len, uint32_t timeout)
{
    int ret = 0;
    xSemaphoreTake(pdev->send_lock, portMAX_DELAY);
    ret = pdev->puart->UART_Send(pdev->puart, (uint8_t *)data, len, timeout);
    xSemaphoreTake(pdev->at_resp_sem, pdMS_TO_TICKS(timeout));
    xSemaphoreGive(pdev->send_lock);
    return ret;
}

/**
 * @brief  发送 AT 指令并等待 OK/ERROR
 * @param  cmdAT 指令字符串（不含 \r\n）
 * @param  timeout_ms 超时时间（毫秒）
 * @return 0成功，其他失败
 */
int at_send_cmd(struct AT_Device *ptDev , char *cmd, uint8_t *resp, uint32_t *resp_len, uint32_t max_len,uint32_t timeout_ms)
{
    struct UART_Device *puart = ptDev->puart;
    uint32_t cur_len;
	uint32_t total_len;
	uint32_t i;
    int ret = -1;
    xSemaphoreTake(ptDev->send_lock, portMAX_DELAY);

    ptDev->resp_line_counts = 0;
    ptDev->resp_status = 0;
    memset(ptDev->resp_line_len, 0, sizeof(ptDev->resp_line_len));
    //xSemaphoreTake(ptDev->at_resp_sem, 0);
    puart->UART_Send(puart, (uint8_t *)cmd, strlen(cmd), timeout_ms);
    // puart->UART_Send(puart, (const uint8_t *)"\r\n", 2);

    if (pdTRUE == xSemaphoreTake(ptDev->at_resp_sem, pdMS_TO_TICKS(timeout_ms)))
    {
        if(resp)
        {
            cur_len=0;
            total_len =0;
            //
            for(i=0;i<ptDev->resp_line_counts;i++)
            {
                cur_len = (ptDev->resp_line_len[i] > max_len)? max_len : ptDev->resp_line_len[i] ;
                //
                if((total_len+cur_len) <= max_len)
                {
                    memcpy(resp+total_len, ptDev->resp[i], cur_len);
                    //
                    total_len += cur_len;
                }
            }
            //
            *resp_len= total_len;
            //
        }
        //
        ret = ptDev->resp_status;
    }

    xSemaphoreGive(ptDev->send_lock);
    return ret;
}
