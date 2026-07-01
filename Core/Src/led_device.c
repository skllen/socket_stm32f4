typedef enum{
	LED_OFF=0,
	LED_ON =1,
}LED_State_Enum;

struct LED_Device {
	  int led_state;
	  int led_init(LED_Device *self,uint32_t pin,uint32_t funx);
	  int control(LED_Device *self. LED_State_Enum state);
	  void *priv_date;
}
static struct LED_Device g_device_led= {
    .led_state   = LED_OFF,
    .control = led_control
};

struct GPIO_Device {
    uint32_t pin;
    uint32_t pinfun;
    
}

static int led_control(struct CmdStruct *self, int argc, char **argv)
{
	PUART_Device uart = Get_UART_Device("stm32_f4_uart1");
    // argv[0] 是第一个参数（例如 "on"），argv[1] 是第二个……
    if (argc >= 1) {
        if (strcmp(argv[0], "on") == 0) {
            // 执行 LED 开操作
					uart->UART_Send(uart,"on",strlen("on"),100);
        } else if (strcmp(argv[0], "off") == 0) {
            // 执行 LED 关操作
					uart->UART_Send(uart,"off",strlen("off"),100);
        }
        // 可处理更多参数
    }
    return 0;
}