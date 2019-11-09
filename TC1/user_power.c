#define os_log(format, ...)  custom_log("OTA", format, ##__VA_ARGS__)
#include "TimeUtils.h"

#include "mico.h"
#include "main.h"
#include "user_udp.h"
#include "user_mqtt_client.h"
#include "user_function.h"
#include "user_power.h"

mico_timer_t power_timer;

PowerRecord power_record = { 1, { 0 } };

/*
static uint32_t clock_count_last = 0;
static uint32_t clock_count = 0;     //纳秒数
static uint32_t timer_count = 0;     //一秒定时器
static uint32_t timer_irq_count = 0; //功率中断数
*/
uint32_t p_count = 0;

char power_record_str[1101] = { 0 };

void SetPowerRecord(PowerRecord* pr, uint32_t pw)
{
    pr->powers[(++pr->idx)% PW_NUM] = pw;
}

char* GetPowerRecord(int idx)
{
    if (idx > power_record.idx) return "";

    int i = idx > 0 ? idx : (power_record.idx - PW_NUM + 1);
    i = i < 0 ? 0 : i;
    char* tmp = power_record_str;
    for (; i <= power_record.idx; i++)
    {
        sprintf(tmp, "%u,", (unsigned int)power_record.powers[i%PW_NUM]);
        tmp += strlen(tmp);
    }
    *(--tmp) = 0;
    return power_record_str;
}

/*
static void PowerTimerHandler(void* arg)
{
    uint32_t timer = 0;

    if (timer_irq_count > 1)
    {
        timer = (clock_count - clock_count_last);

        if (timer_count > 3)
        {
            timer /= 1000;
            timer += 4294967; //0xffffffff/1000;
        }
        else if (clock_count < clock_count_last)
        {
            timer += 0xffffffff;
            timer /= 1000;
        }
        else
        {
            timer /= 1000;
        }
        power = 17100000 * (timer_irq_count - 1) / timer;
        timer_count = 0;
        timer_irq_count = 0;
    }
    else
    {
        timer_count++;
    }
}
*/

float n_1s = 0;            //功率中断次数
mico_time_t t_x = 0;       //当前秒*1000
mico_time_t past_ms = 0;   //系统运行的毫秒数
mico_time_t rest_x_ms = 0; //距离当前秒走过的毫秒数
mico_time_t rest_y_ms = 0; //距离下一秒差的秒数

static void PowerIrqHandler(void* arg)
{
    //clock_count = mico_nanosecond_clock_value();
    //if (timer_irq_count == 0) clock_count_last = clock_count;
    //timer_irq_count++;

    p_count++;

    mico_time_get_time(&past_ms);
    if (t_x == 0)
    {
        t_x = past_ms - 1;
    }
    rest_x_ms = past_ms - t_x;
    if (rest_x_ms <= 1000)
    {
        n_1s += 1;
        rest_y_ms = t_x + 1000 - past_ms;
    }
    else if (rest_x_ms > 1000 && rest_x_ms < 2000)
    {
        n_1s += (float)rest_y_ms / (rest_x_ms - 1000 + rest_y_ms);
        rest_y_ms = 2000 - rest_x_ms;
        t_x = past_ms / 1000 * 1000;

        float power2 = 17.1 * n_1s;
        SetPowerRecord(&power_record, (int)power2);
        n_1s = (float)(rest_x_ms - 1000) / (rest_x_ms - 1000 + rest_y_ms);
    }
    else
    {
        //一般不会出现这个情况, 所以不管了...哈哈哈~
        SetPowerRecord(&power_record, 123456);
        SetPowerRecord(&power_record, past_ms);
        SetPowerRecord(&power_record, t_x);
        SetPowerRecord(&power_record, rest_x_ms);
    }
}

void PowerInit(void)
{
    os_log("user_power_init");

    MicoGpioInitialize(POWER, INPUT_PULL_UP);
    //mico_rtos_init_timer(&power_timer, 1000, PowerTimerHandler, NULL);
    mico_rtos_start_timer(&power_timer);

    MicoGpioEnableIRQ(POWER, IRQ_TRIGGER_FALLING_EDGE, PowerIrqHandler, NULL);
}

