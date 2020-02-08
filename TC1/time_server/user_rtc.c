#include "http_server/web_log.h"
#define os_log(format, ...) do { custom_log("RTC", format, ##__VA_ARGS__); web_log(format, ##__VA_ARGS__) } while(0)

#include "main.h"
#include "user_gpio.h"
#include "sntp.h"
#include "c_json/c_json.h"
#include "mqtt_server/user_mqtt_client.h"
#include "mqtt_server/user_function.h"

void rtc_thread(mico_thread_arg_t arg);

OSStatus user_sntp_get_time()
{
    OSStatus err = kNoErr;
    ntp_timestamp_t current_time;

    struct hostent * hostent_content = NULL;
    char ** pptr = NULL;
    struct in_addr ipp;

    ipp.s_addr = 0xd248912c;
    err = sntp_get_time(&ipp, &current_time);

    if (err != kNoErr)
    {
        int ntp_count = 5;
        char* ntp_hosts[5] = {
            "pool.ntp.org",
            "cn.ntp.org.cn",
            "cn.pool.ntp.org",
            "s1a.time.edu.cn",
            "ntp.sjtu.edu.cn",
        };

        int i = 0;
        for (; i < ntp_count; i++)
        {
            hostent_content = gethostbyname(ntp_hosts[i]);
            if (hostent_content == NULL)
            {
                os_log("gethostbyname(%s)", ntp_hosts[i]);
                continue;
            }
            pptr = hostent_content->h_addr_list;
            ipp.s_addr = *(uint32_t *)(*pptr);
            err = sntp_get_time(&ipp, &current_time);
            if (err == kNoErr)
            {
                break;
            }
        }
    }

    if (err != kNoErr)
    {
        os_log("sntp_get_time4 err[%d]", err);
        return err;
    }

    mico_utc_time_ms_t utc_time_ms = (uint64_t)current_time.seconds * (uint64_t)1000
        + (current_time.microseconds / 1000);
    mico_time_set_utc_time_ms(&utc_time_ms);
    return kNoErr;
}

OSStatus user_rtc_init(void)
{
    OSStatus err = kNoErr;

//  mico_rtc_time_t rtc_time;
//  rtc_time.sec = 0;
//  rtc_time.min = 0;
//  rtc_time.hr = 0;
//
//  rtc_time.date = 1;
//  rtc_time.weekday = 1;
//  rtc_time.month = 1;
//  rtc_time.year = 1;
//
//  MicoRtcSetTime(&rtc_time);

//  /* create mqtt msg send queue */
//  err = mico_rtos_init_queue(&mqtt_msg_send_queue, "mqtt_msg_send_queue", sizeof(p_mqtt_send_msg_t),
//                              MAX_MQTT_SEND_QUEUE_SIZE);
//  require_noerr_action(err, exit, app_log("ERROR: create mqtt msg send queue err=%d.", err));

    /* start rtc client */
    err = mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "rtc",
                                   (mico_thread_function_t) rtc_thread,
                                   0x1000, 0);
    require_noerr_string(err, exit, "ERROR: Unable to start the rtc thread.");

    if (kNoErr != err) os_log("ERROR1, app thread exit err: %d kNoErr[%d]", err, kNoErr);

    exit:
    return err;
}

void rtc_thread(mico_thread_arg_t arg)
{
    int i, j;
    char task_flag[SOCKET_NUM] = { -1, -1, -1, -1, -1, -1 };   //记录每个插座哪个任务需要返回数据
    OSStatus err = kUnknownErr;
    LinkStatusTypeDef LinkStatus;
    mico_rtc_time_t rtc_time;


    mico_utc_time_t utc_time;
    mico_utc_time_t utc_time_last = 0;
    while (1)
    {   //上电后连接了wifi才开始走时否则等待连接
        micoWlanGetLinkStatus(&LinkStatus);
        if (LinkStatus.is_connected == 1)
        {
            err = user_sntp_get_time();
            if (err == kNoErr)
            {
                os_log("sntp success!");
                rtc_init = 1;
                break;
            }
        }

        mico_rtos_thread_sleep(3);
    }

    while (1)
    {
        mico_time_get_utc_time(&utc_time);
        utc_time += 28800;

        if (utc_time_last != utc_time)
        {
            utc_time_last = utc_time;
            total_time++;
        }

        struct tm * currentTime = localtime((const time_t *) &utc_time);
        rtc_time.sec = currentTime->tm_sec;
        rtc_time.min = currentTime->tm_min;
        rtc_time.hr = currentTime->tm_hour;

        rtc_time.date = currentTime->tm_mday;
        rtc_time.weekday = currentTime->tm_wday;
        rtc_time.month = currentTime->tm_mon + 1;
        rtc_time.year = (currentTime->tm_year + 1900) % 100;

        // MicoRtcSetTime(&rtc_time);      //MicoRtc不自动走时!

        //if (rtc_time.sec == 0)
        //os_log("time1:20%02d/%02d/%02d %d %02d:%02d:%02d",rtc_time.year,rtc_time.month,rtc_time.date,rtc_time.weekday,rtc_time.hr,rtc_time.min,rtc_time.sec);

        char update_user_config_flag = 0;
        for (i = 0; i < SOCKET_NUM; i++)
        {
            for (j = 0; j < SOCKET_TIME_TASK_NUM; j++)
            {
                if (user_config->socket[i].task[j].on != 0)
                {

                    char repeat = user_config->socket[i].task[j].repeat;
                    if ( //符合条件则改变继电器状态: 秒为0 时分符合设定值, 重复符合设定值
                    rtc_time.sec == 0 && rtc_time.min == user_config->socket[i].task[j].minute
                    && rtc_time.hr == user_config->socket[i].task[j].hour
                    && ((repeat == 0x00) || repeat & (1 << (rtc_time.weekday - 1)))
                  )
                    {
                        if (user_config->socket[i].on != user_config->socket[i].task[j].action)
                        {
                            UserRelaySet(i, user_config->socket[i].task[j].action);
                            update_user_config_flag = 1;
                            user_mqtt_send_socket_state(i);
                        }
                        if (repeat == 0x00)
                        {
                            task_flag[i] = j;
                            user_config->socket[i].task[j].on = 0;
                            update_user_config_flag = 1;
                        }
                    }
                }
            }
        }

        //更新储存数据 更新定时任务数据
        if (update_user_config_flag == 1)
        {
            os_log("update_user_config_flag");
            mico_system_context_update(sys_config);
            update_user_config_flag = 0;

            cJSON *json_send = cJSON_CreateObject();
            cJSON_AddStringToObject(json_send, "mac", strMac);

            for (i = 0; i < SOCKET_NUM; i++)
            {
                char strTemp1[] = "socket_X";
                strTemp1[5] = i + '0';
                cJSON *json_send_socket = cJSON_CreateObject();
                cJSON_AddNumberToObject(json_send_socket, "on", user_config->socket[i].on);

                if (task_flag[i] >= 0)
                {
                    cJSON *json_send_socket_setting = cJSON_CreateObject();

                    j = task_flag[i];
                    char strTemp2[] = "task_X";
                    strTemp2[5] = j + '0';
                    cJSON *json_send_socket_task = cJSON_CreateObject();
                    cJSON_AddNumberToObject(json_send_socket_task, "hour", user_config->socket[i].task[j].hour);
                    cJSON_AddNumberToObject(json_send_socket_task, "minute", user_config->socket[i].task[j].minute);
                    cJSON_AddNumberToObject(json_send_socket_task, "repeat", user_config->socket[i].task[j].repeat);
                    cJSON_AddNumberToObject(json_send_socket_task, "action", user_config->socket[i].task[j].action);
                    cJSON_AddNumberToObject(json_send_socket_task, "on", user_config->socket[i].task[j].on);
                    cJSON_AddItemToObject(json_send_socket_setting, strTemp2, json_send_socket_task);

                    cJSON_AddItemToObject(json_send_socket, "setting", json_send_socket_setting);

                    task_flag[i] = -1;
                }
                cJSON_AddItemToObject(json_send, strTemp1, json_send_socket);
            }

            char *json_str = cJSON_Print(json_send);
            user_send(false, json_str);    //发送数据

            free(json_str);
            cJSON_Delete(json_send);
//          os_log("cJSON_Delete");
        }

        //SNTP服务 开机及每小时校准一次
        if (rtc_init != 1 || (rtc_time.sec == 0 && rtc_time.min == 0))
        {
            micoWlanGetLinkStatus(&LinkStatus);
            if (LinkStatus.is_connected == 1)
            {
                err = user_sntp_get_time();
                if (err == kNoErr)
                    rtc_init = 1;
                else
                    rtc_init = 2;
            }
        }



        mico_rtos_thread_msleep(900);
    }

//  exit:
    os_log("EXIT: rtc exit with err = %d.", err);
    mico_rtos_delete_thread(NULL);
}

