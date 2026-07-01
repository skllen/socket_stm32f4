#include "data_event.h"

#define INPUT_QUEUE_SIZE 100 //input event queue max len

static struct inputEvent g_inputdata[INPUT_QUEUE_SIZE];
static uint32_t r ;
static uint32_t w ;
static uint32_t count ;

static struct outputEvent *g_processList;
void inputEvent_Init(void)
{
    r = 0;
    w = 0;
    count = 0;
    memset(g_inputdata, 0, sizeof(g_inputdata));
}

int inputEvent_Read(struct inputEvent *ev)
{
    if (ev == NULL) {
        return -1;
    }

    /* 环形缓冲区为空 */
    if (count == 0) {
        return -2;
    }

    *ev = g_inputdata[r];

    r = (r + 1) % INPUT_QUEUE_SIZE;
    count--;

    return 0;   
}

int inputEvent_Write(const struct inputEvent *ev)
{
    if (ev == NULL) {
        return -1;
    }

    /* 环形缓冲区已满 */
    if (count >= INPUT_QUEUE_SIZE) {
        return -2;
    }

    g_inputdata[w] = *ev;

    w = (w + 1) % INPUT_QUEUE_SIZE;
    count++;

    return 0;
}

int outputEvent_Execut(struct inputEvent *ev)
{
    struct outputEvent *pnode = g_processList;
    while (pnode != NULL)
    {
        /* code */
        if(ev->type == pnode->type)
        {
            pnode->Execut_Callback(ev);
            return 0;
        }
        pnode = pnode->next;
    }
    return 1;
}

int outputEvent_register(struct outputEvent *ev)
{
    if (ev == NULL || ev->Execut_Callback == NULL) {
        return -1;
    }
    ev->next = g_processList;
    g_processList = ev;
    return 0;
}