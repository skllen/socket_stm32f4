#include "cmd_device.h"
#include "uart_device.h"
#include <stdio.h>          // for sscanf (if needed, but we'll use direct argv)
#include <string.h>
#include <stdint.h>
#include "gpio.h"
/* cmd_devices.c */
#include "cmd_device.h"
#include "uart_device.h"   /* Get_UART_Device 等 */
#include "data_event.h"
/* ===== 前置声明回调 ===== */
static int led_control(struct Cmd_Device *self, int argc, char **argv);
static int oled_control(struct Cmd_Device *self, int argc, char **argv);
static int Uart_Execut_Callback(struct inputEvent *ev);
	
/* ===== 设备实例 ===== */
static struct Cmd_Device g_cmd_led = {
    .dev_name     = "cmd:led",
    .init         = NULL,
    .Cmd_Callback = led_control,
    .pri_data     = NULL,
};

static struct Cmd_Device g_cmd_oled = {
    .dev_name     = "cmd:oled",
    .init         = NULL,
    .Cmd_Callback = oled_control,
    .pri_data     = NULL,
};

/* ===== 设备表（指针数组） ===== */
static struct Cmd_Device *g_cmds[] = { &g_cmd_led, &g_cmd_oled };
#define CMD_COUNT  (sizeof(g_cmds) / sizeof(g_cmds[0]))

struct Cmd_Device *Get_Cmd(char *name)
{
    if (name == NULL) return NULL;

    for (int i = 0; i < (int)CMD_COUNT; i++) {
        if (strcmp(g_cmds[i]->dev_name, name) == 0) {
            return g_cmds[i];
        }
    }
    return NULL;   /* 没找到 */
}

/* ===== 回调实现 ===== */
static int led_control(struct Cmd_Device *self, int argc, char **argv)
{
    PUART_Device puart = Get_UART_Device("stm32_f4_uart1");
    struct inputEvent  ev;
    if (argc < 1) return -1;

    if (strcmp(argv[0], "on") == 0) {
		//	HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_RESET);
        //puart->UART_Send(puart, (uint8_t *)"led on\r\n", strlen("led on\r\n"), 100);

    } else if (strcmp(argv[0], "off") == 0) {
		//	HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_SET);
        //puart->UART_Send(puart, (uint8_t *)"led off\r\n", strlen("led off\r\n"), 100);
    } else {
        return -1;
    }
    return 0;
}

static int oled_control(struct Cmd_Device *self, int argc, char **argv)
{
    PUART_Device puart = Get_UART_Device("stm32_f4_uart1");
    if (puart == NULL) return -1;
    if (argc < 1) return -1;

    /* 把所有参数用逗号拼接后回显 */
    for (int i = 0; i < argc; i++) {
        puart->UART_Send(puart, (uint8_t *)argv[i], strlen(argv[i]), 100);
        if (i < argc - 1) {
            puart->UART_Send(puart, (uint8_t *)",", 1, 100);
        }
    }
    //puart->UART_Send(puart, (uint8_t *)"\r\n", 2, 20);

    return 0;
}

static struct outputEvent g_Uart_Output_Event = {
  .type = 0x01,
  .Execut_Callback = Uart_Execut_Callback
  /* data */
};

/* ===== 解析一行命令并派发 ===== */
static int Uart_Execut_Callback(struct inputEvent *ev)
{
  char *buffer = ev->buf;
  int len = ev->data_len;
      /* 1. 找到第一个逗号，切出设备名 */
    char *comma = strchr(buffer, ',');
    char  dev_name[64];

    if (comma != NULL) {
        int name_len = comma - buffer;
        if (name_len >= sizeof(dev_name)) return 1;
        memcpy(dev_name, buffer, name_len);
        dev_name[name_len] = '\0';
    } else {
        /* 没有逗号，整行就是设备名（无参数） */
        strncpy(dev_name, buffer, sizeof(dev_name) - 1);
        dev_name[sizeof(dev_name) - 1] = '\0';
    }

    /* 2. 通过干净的设备名获取设备 */
    struct Cmd_Device *dev = Get_Cmd(dev_name);
    if (dev == NULL) {
        return 1;
    }

    /* 3. 提取参数部分 */
    int   argc = 0;
    char *argv[CMD_ARGV_MAX];

    if (comma != NULL) {
        char *args = comma + 1;  /* 逗号后面就是参数 */
        char *token = strtok(args, ",");
        while (token != NULL && argc < CMD_ARGV_MAX) {
            argv[argc++] = token;
            token = strtok(NULL, ",");
        }
    }

    /* 4. 派发 */
    dev->Cmd_Callback(dev, argc, argv);
  return 0;
}

/* ===== 串口命令接收任务 ===== */
void process_cmd_task(PUART_Device uart)
{
  static char line[CMD_LINE_MAX];
  static int32_t data_len;
  static char cur_data;
  static char last_data;
  struct inputEvent ev;

  if (0 == uart->UART_Recv(uart, (uint8_t *)&cur_data, 1))
  {
      //					uart->UART_Send(uart, (uint8_t *)&cur_data, 1,1);
      if (cur_data == '\r' && last_data == '\n')
      {
        /* '\n' 已经存在 line[data_len-1]，去掉它 */
        if (data_len > 0)
          data_len--;
        line[data_len] = '\0';
        memcpy(ev.buf, line, data_len);
				ev.buf[data_len] = '\0';
        ev.data_len = data_len;
				ev.type = 1;
        inputEvent_Write(&ev);
        // process_line_cmd(line, data_len);
        data_len = 0;
        last_data = 0;
      }
      else
      {
        if (data_len < CMD_LINE_MAX - 1)
        {
          line[data_len++] = cur_data;
        }
        last_data = cur_data;
      }
  }
}

int cmd_init(void)
{
      outputEvent_register(&g_Uart_Output_Event);
}
