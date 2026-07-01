/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : FreeRTOS UART + KEY + LED + OLED 控制系统
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "queue.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "uart_device.h"
#include "driver_oled.h"
/* USER CODE END Includes */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define CMD_LINE_MAX                64
#define INPUT_BUFF_MAX_LEN          64

#define OLED_MSG_LEN                16
#define UART_CMD_QUEUE_LENGTH       10
#define KEY_EVENT_QUEUE_LENGTH      20
#define OLED_SHOW_QUEUE_LENGTH      10
#define LED_QUEUE_LENGTH            50

/* LED控制码定义 */
#define LED_CMD_OFF     0x01
#define LED_CMD_ON      0x81
#define LED_CMD_TOGGLE  0x41

/* 任务栈大小和优先级 */
#define UART_PARSE_TASK_STACK_SIZE  256
#define KEY_CHECK_TASK_STACK_SIZE   128
#define INPUT_TASK_STACK_SIZE       256
#define LED_TASK_STACK_SIZE         128
#define OLED_TASK_STACK_SIZE        256

#define UART_PARSE_TASK_PRIORITY    (osPriorityNormal)
#define KEY_CHECK_TASK_PRIORITY     (osPriorityNormal)
#define INPUT_TASK_PRIORITY         (osPriorityAboveNormal)
#define LED_TASK_PRIORITY           (osPriorityNormal)
#define OLED_TASK_PRIORITY          (osPriorityNormal)
/* USER CODE END PD */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* GPIO翻转宏定义 */
#define HAL_GPIO_TogglePin(GPIOX, GPIO_PIN) \
    do{HAL_GPIO_WritePin(GPIOX, GPIO_PIN, !HAL_GPIO_ReadPin(GPIOX, GPIO_PIN)); }while(0)

/* 输入事件结构体 */
struct inputEvent {
    int type;                           // 消息类型: 0-按键, 1-UART命令, 2-用户自定义
    char buf[INPUT_BUFF_MAX_LEN];      // 数据缓冲区
    int data_len;                       // 数据长度
    int code;                           // 设备代码 (1-LED1, 2-LED2...)
    int value;                          // 控制值或扩展参数
};


/* USER CODE END PTD */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* 队列句柄 */
static QueueHandle_t g_uartcmd_queue = NULL;   // UART命令队列
static QueueHandle_t g_oled_queue = NULL;      // OLED显示队列
static QueueHandle_t g_led_queue = NULL;       // LED控制队列
static QueueHandle_t g_key_queue = NULL;       // 按键事件队列

static QueueSetHandle_t g_queue_set = NULL;    // 队列集

/* 任务句柄 */
static TaskHandle_t g_uart_parse_handle = NULL;
static TaskHandle_t g_key_check_handle = NULL;
static TaskHandle_t g_input_handle = NULL;
static TaskHandle_t g_led_handle = NULL;
static TaskHandle_t g_oled_show_handle = NULL;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

void uart_parse_task(void *arg);
void key_check_task(void *arg);
void Input_Task(void *arg);
void led_control_task(void *arg);
void oled_show_task(void *arg);

static int SendToOledQueue(const char* msg, int type, int value);
static int SendToLedQueue(uint8_t control);
static int Uart_Execut_Callback(struct inputEvent *ev);
static int LED_Execut_Callback(struct inputEvent *ev);

/* USER CODE END FunctionPrototypes */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ==================== 辅助函数 ==================== */

/**
 * 发送消息到OLED队列
 * @param msg: 要显示的字符串
 * @param type: 消息类型 (0-按键, 1-UART命令, 2-用户自定义)
 * @param value: 扩展参数（预留）
 * @return: 0-成功, -1-失败
 */
static int SendToOledQueue(const char* msg, int type, int value) {
    struct inputEvent ev;
    ev.type = type;
    ev.value = value;
    
    // 安全复制字符串
    size_t msg_len = strlen(msg);
    if (msg_len >= INPUT_BUFF_MAX_LEN) {
        msg_len = INPUT_BUFF_MAX_LEN - 1;
    }
    
    memcpy(ev.buf, msg, msg_len);
    ev.buf[msg_len] = '\0';  // 确保字符串结束
    ev.data_len = msg_len;
     if (type == 0) {
        // 如果队列满了，先清空一个位置
        if (uxQueueSpacesAvailable(g_oled_queue) == 0) {
            struct inputEvent dummy;
            xQueueReceive(g_oled_queue, &dummy, 0);  // 丢弃最旧的消息
        }
        xQueueSendToFront(g_oled_queue, &ev, 0);  // 插入队首，立即处理
        return 0;
    }
    if (xQueueSend(g_oled_queue, &ev, 0) != pdPASS) {
        return -1;
    }
    return 0;
}

/**
 * 发送LED控制码到LED队列
 * @param control: LED控制码
 * @return: 0-成功, -1-失败
 */
static int SendToLedQueue(uint8_t control) {
    if (xQueueSend(g_led_queue, &control, pdMS_TO_TICKS(50)) != pdPASS) {
        return -1;
    }
    return 0;
}

/* ==================== UART命令解析 ==================== */

/**
 * 解析UART命令并分发
 * 命令格式: led,on / led,off / led,toggle / oled,hello
 * @param ev: 输入事件结构体
 * @return: 0-成功, -1-失败
 */
/**
 * UART命令解析（在这里去除cmd:前缀）
 */
static int Uart_Execut_Callback(struct inputEvent *ev)
{
    char *buffer = ev->buf;
    int len = ev->data_len;
    
    // 安全检查
    if (buffer == NULL || len <= 0 || len >= INPUT_BUFF_MAX_LEN) {
        return -1;
    }
    
    // 确保字符串结束
    buffer[len] = '\0';
    
    // ===== 在这里处理 "cmd:" 前缀 =====
    if (len >= 4 && strncmp(buffer, "cmd:", 4) == 0) {
        // 跳过 "cmd:" 前缀
        buffer += 4;
        len -= 4;
    }
    
    // 去除首尾空白字符
    while (len > 0 && (*buffer == ' ' || *buffer == '\t')) {
        buffer++;
        len--;
    }
    
    while (len > 0 && (buffer[len-1] == ' ' || buffer[len-1] == '\t')) {
        len--;
        buffer[len] = '\0';
    }
    
    if (len <= 0 || strlen(buffer) == 0) {
        return -1;
    }
    
    /* 显示清理后的命令 */
    char displayMsg[INPUT_BUFF_MAX_LEN];
    snprintf(displayMsg, sizeof(displayMsg), "cmd:%s", buffer);
    SendToOledQueue(displayMsg, 1, 0);
    
    /* 解析设备名和参数 */
    char dev_name[32] = {0};
    char param[32] = {0};
    
    char *comma = strchr(buffer, ',');
    
    if (comma != NULL) {
        // 有参数: device,param
        int name_len = comma - buffer;
        if (name_len >= sizeof(dev_name)) {
            return -1;
        }
        
        memcpy(dev_name, buffer, name_len);
        dev_name[name_len] = '\0';
        
        // 复制参数，跳过逗号
        size_t param_len = len - name_len - 1;
        if (param_len >= sizeof(param)) {
            param_len = sizeof(param) - 1;
        }
        memcpy(param, comma + 1, param_len);
        param[param_len] = '\0';
        
    } else {
        // 无参数
        strncpy(dev_name, buffer, sizeof(dev_name) - 1);
        dev_name[sizeof(dev_name) - 1] = '\0';
    }
    
    /* 根据设备类型分发 */
    if (strcmp(dev_name, "led") == 0) {
        uint8_t led_cmd = 0;
        
        if (strcmp(param, "on") == 0) {
            led_cmd = LED_CMD_ON;
//            SendToLedQueue("LED_CMD_ON", 2, 0);
        }
        else if (strcmp(param, "off") == 0) {
            led_cmd = LED_CMD_OFF;
//            SendToLedQueue("LED_CMD_OFF", 2, 0);
        }
        else if (strcmp(param, "toggle") == 0) {
            led_cmd = LED_CMD_TOGGLE;
//            SendToLedQueue("LED_CMD_TOGGLE", 2, 0);
        }
        else {
            //SendToOledQueue("bad param", 2, 0);
            return -1;
        }
        
        SendToLedQueue(led_cmd);
        return 0;
    }
    else if (strcmp(dev_name, "oled") == 0) {
        if (strlen(param) > 0) {
            //SendToOledQueue(param, 2, 0);
        }
        return 0;
    }
    else {
//        SendToOledQueue("bad device", 2, 0);
        return -1;
    }
}


/* ==================== LED控制回调 ==================== */

/**
 * LED硬件控制函数
 * @param ev: 输入事件结构体
 * @return: 0-成功, -1-失败
 */
static int LED_Execut_Callback(struct inputEvent *ev)
{
    if (ev->code == 1) {
        // LED1控制 (GPIOF PIN10)
        if (ev->value == LED_CMD_OFF) {
            HAL_GPIO_WritePin(GPIOF, GPIO_PIN_10, GPIO_PIN_SET);
        }
        else if (ev->value == LED_CMD_ON) {
            HAL_GPIO_WritePin(GPIOF, GPIO_PIN_10, GPIO_PIN_RESET);
        }
        else if (ev->value == LED_CMD_TOGGLE) {
            HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_10);
        }
        return 0;
    }
    else if (ev->code == 2) {
        // LED2控制 (GPIOF PIN9)
        if (ev->value == LED_CMD_OFF) {
            HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_SET);
        }
        else if (ev->value == LED_CMD_ON) {
            HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_RESET);
        }
        else if (ev->value == LED_CMD_TOGGLE) {
            HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_9);
        }
        return 0;
    }
    
    return -1;
}

/* ==================== UART解析任务 ==================== */

/**
 * UART解析任务：接收串口字符，组装完整命令
 * 命令格式: xxx,yyy\n\r
 */
void uart_parse_task(void *arg)
{
     static char line[CMD_LINE_MAX];
    volatile static int32_t data_len = 0;
    volatile static char cur_data;
    volatile static char last_data = 0;
     struct inputEvent ev;
    
    PUART_Device uart = Get_UART_Device("stm32_f4_uart1");
    if (uart == NULL) {
        vTaskDelete(NULL);
    }
    
    uart->UART_Init(uart, 115200, 8, 'N', 1);
    
    while (1)
    {
        // 接收一个字符
        if (0 == uart->UART_Recv(uart, (uint8_t *)&cur_data, 1))
        {
            // 检测命令结束符: \n\r
            if (cur_data == '\r' && last_data == '\n')
            {
                // 去掉已存入的 \n
                if (data_len > 0) {
                    data_len--;
                }
                
                if (data_len > 0) {
                    line[data_len] = '\0';
                    
                    // 构建输入事件
                    ev.type = 1;  // UART类型
                    memcpy(ev.buf, line, data_len);
                    ev.buf[data_len] = '\0';
                    ev.data_len = data_len+1;
                    if(ev.data_len >16)
										{
											ev.data_len =16;
										}
                    // 发送到INPUT任务
                    xQueueSend(g_uartcmd_queue, &ev, 0);
//										vTaskDelay(100);
                }
                
                // 重置缓冲区
                data_len = 0;
                last_data = 0;
								line[0] = '\0';
            }
            else
            {
                // 普通字符，累积到缓冲区
                if (data_len < CMD_LINE_MAX - 1) {
                    line[data_len++] = cur_data;
                } else {
                    // 缓冲区溢出，重置
                    data_len = 0;
                }
                last_data = cur_data;
            }
        }
    }
}

/* ==================== 按键检测任务 ==================== */

/**
 * 按键检测任务：轮询方式检测按键，带消抖
 * 按键引脚: GPIOB PIN1
 */
void key_check_task(void *arg)
{
    uint8_t key_state = 0;  // 0-未按下, 1-已按下
		uint8_t led_toggle_state = 0;
    uint8_t event_msg;
    
    for (;;)
    {
        /* 当前状态是空闲（未按下） */
        if (key_state == 0) {
            if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_RESET) {
                vTaskDelay(5);  // 消抖延时20ms
                if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_RESET) {
                    // 确认按下
                    key_state = 1;
                    event_msg = 1;  // 按下事件
                    xQueueSend(g_key_queue, &event_msg, 0);

//									       // 按键按下 - type=0，显示在第0行
//									       SendToOledQueue("key pressed", 0, 0);
//									       
//									       // 切换LED状态
//									       if (led_toggle_state == 0) {
//									           // 当前是灭，切换到亮
//									           SendToLedQueue(LED_CMD_ON);
//									           led_toggle_state = 1;
//									       } else {
//									           // 当前是亮，切换到灭
//									           SendToLedQueue(LED_CMD_OFF);
//									           led_toggle_state = 0;
//									        }						
									
                }
            }
        }
        /* 当前状态是按下 */
        else if (key_state == 1) {
            if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_SET) {
                vTaskDelay(5);  // 消抖延时20ms
                if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_SET) {
                    // 确认释放
                    key_state = 0;
                    event_msg = 0;  // 释放事件
											// 按键释放 - type=0，显示在第0行
//									    SendToOledQueue("key released", 0, 0);
                    xQueueSend(g_key_queue, &event_msg, 0);
                }
            }
        }
        
        vTaskDelay(1);  // 轮询间隔10ms
    }
}

/* ==================== INPUT任务（核心） ==================== */

/**
 * INPUT任务：统一处理UART和按键输入事件
 * 使用队列集同时监听多个队列
 */
void Input_Task(void *arg)
{
    QueueHandle_t handle;
    struct inputEvent ev;
    uint8_t key;
    static uint8_t led_toggle_state = 0;  // LED翻转状态: 0-灭, 1-亮
    
    for (;;)
    {
        // 等待队列集中的任何队列有数据
        handle = xQueueSelectFromSet(g_queue_set, portMAX_DELAY);
        
        if (handle == g_uartcmd_queue) {
            // ========== UART命令事件 ==========
            if (xQueueReceive(handle, &ev, 0) == pdTRUE) {
                // 解析并执行UART命令
                Uart_Execut_Callback(&ev);
            }
        }
        else if (handle == g_key_queue) {
            // ========== 按键事件 ==========
            if (xQueueReceive(handle, &key, 0) == pdTRUE) {
                
                if (key == 1) {
                    // 按键按下 - type=0，显示在第0行
                    SendToOledQueue("key pressed", 0, 0);
                    
                    // 切换LED状态
                    if (led_toggle_state == 0) {
                        // 当前是灭，切换到亮
                        SendToLedQueue(LED_CMD_ON);
                        led_toggle_state = 1;
                    } else {
                        // 当前是亮，切换到灭
                        SendToLedQueue(LED_CMD_OFF);
                        led_toggle_state = 0;
                    }
                }
                else if (key == 0) {
                    // 按键释放 - type=0，显示在第0行
                    SendToOledQueue("key released", 0, 0);
                }
            }
        }
    }
}

/* ==================== LED控制任务 ==================== */

/**
 * LED控制任务：从队列读取控制码，执行硬件操作
 */
void led_control_task(void *arg)
{
    uint8_t control;
    struct inputEvent ev;

    for (;;)
    {
        // 等待LED控制命令
        if (xQueueReceive(g_led_queue, &control, portMAX_DELAY) == pdTRUE) {
            // 构建事件结构
            ev.type = 0x01;
            ev.code = 1;  // 默认控制LED1 (GPIOF PIN10)
            ev.value = control;
            
            // 执行LED控制
            LED_Execut_Callback(&ev);
        }
        
        vTaskDelay(20);
    }
}

/* ==================== OLED显示任务 ==================== */

/**
 * OLED显示任务：从队列读取消息，显示到OLED屏幕
 * 显示布局:
 *   第0行: 按键消息
 *   第2行: UART命令
 *   第4行: 用户自定义内容
 *   第6行: 其他消息
 */
void oled_show_task(void *arg)
{
    OLED_Init();
    OLED_Clear();
    
    struct inputEvent msg;
	//OLED_PrintString(0, 0, "cmd:hello,1111111111111");
    for (;;)
    {
        if (xQueueReceive(g_oled_queue, &msg, portMAX_DELAY) == pdTRUE) {
            
            // 确保字符串结束
            if (msg.data_len >= INPUT_BUFF_MAX_LEN) {
                msg.data_len = INPUT_BUFF_MAX_LEN - 1;
            }
            msg.buf[msg.data_len] = '\0';
						if(msg.data_len >=16)
						{
//							msg.data_len =15;
							msg.buf[15]='\0';
						}
            
            // 根据消息类型显示到不同行
            uint8_t line_y = 0;
            
            switch (msg.type) {
                case 0:  // 按键消息 -> 第0行
                    line_y = 0;
                    OLED_ClearLine(0, line_y);  // 清除该行
                    OLED_PrintString(0, line_y, msg.buf);
                    break;
                    
                case 1:  // UART命令 -> 第2行
                    line_y = 2;
                    OLED_ClearLine(0, line_y);
								OLED_ClearLine(0, line_y+1);
                    OLED_PrintString(0, line_y, msg.buf);
                    break;
            }
        }
        
        vTaskDelay(100);
    }
}

/* ==================== 系统初始化 ==================== */

/**
 * 创建所有队列和任务
 */
void StartTask(void)
{
    /* 创建输入队列 */
    g_uartcmd_queue = xQueueCreate(UART_CMD_QUEUE_LENGTH, sizeof(struct inputEvent));
    g_key_queue = xQueueCreate(KEY_EVENT_QUEUE_LENGTH, sizeof(uint8_t));
    
    /* 创建队列集 */
    g_queue_set = xQueueCreateSet(UART_CMD_QUEUE_LENGTH + KEY_EVENT_QUEUE_LENGTH);
    xQueueAddToSet(g_uartcmd_queue, g_queue_set);
    xQueueAddToSet(g_key_queue, g_queue_set);
    
    /* 创建输出队列 */
    g_led_queue = xQueueCreate(LED_QUEUE_LENGTH, sizeof(uint8_t));
    g_oled_queue = xQueueCreate(OLED_SHOW_QUEUE_LENGTH, sizeof(struct inputEvent));
    
    /* 创建任务 */
    xTaskCreate(
        uart_parse_task,
        "uart_parse",
        UART_PARSE_TASK_STACK_SIZE,
        NULL,
        UART_PARSE_TASK_PRIORITY-1,
        &g_uart_parse_handle);
    
    xTaskCreate(
        key_check_task,
        "key_check",
        KEY_CHECK_TASK_STACK_SIZE,
        NULL,
        KEY_CHECK_TASK_PRIORITY+2,
        &g_key_check_handle);
    
    xTaskCreate(
        Input_Task,
        "input",
        INPUT_TASK_STACK_SIZE,
        NULL,
        INPUT_TASK_PRIORITY,
        &g_input_handle);
    
    xTaskCreate(
        led_control_task,
        "led_ctrl",
        LED_TASK_STACK_SIZE,
        NULL,
        LED_TASK_PRIORITY,
        &g_led_handle);
    
    xTaskCreate(
        oled_show_task,
        "oled_show",
        OLED_TASK_STACK_SIZE,
        NULL,
        OLED_TASK_PRIORITY+1,
        &g_oled_show_handle);
}