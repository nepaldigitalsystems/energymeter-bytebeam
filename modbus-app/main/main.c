/*******************************************************************************
 * (C) Copyright 2018-2023; Nepal Digital Systems Pvt Ltd, Kathmandu, Nepal.
 * The attached material and the information contained therein is proprietary
 * to Nepal Digital Systems Pvt Ltd and is issued only under strict confidentiality
 * arrangements.It shall not be used, reproduced, copied in whole or in part,
 * adapted,modified, or disseminated without a written license of Nepal Digital
 * Systems Pvt Ltd.It must be returned to Nepal Digital Systems Pvt Ltd upon
 * its first request.
 *
 *  File Name           : main.c
 *
 *  Description         : Main application file modbus, WiFi Connection and ByteBeam integration
 *
 *  Change history      :
 *
 *     Author        Date          Ver                 Description
 *  ------------    --------       ---   --------------------------------------
 *  Lomas Subedi  30 March 2023    1.0               Initial Creation
 *
 *******************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_tls.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "modbus_params.h" // for modbus parameters structures
#include "mbcontroller.h"
#include "sdkconfig.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "cJSON.h"

// #include "protocol_examples_common.h"

#include "bytebeam_sdk.h"


#define PROV_TRANSPORT_SOFTAP 1
#define CONFIG_EXAMPLE_PROV_TRANSPORT_SOFTAP 1
#define CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE 1

/* Signal Wi-Fi events on this event-group */
const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;

// #define EXAMPLE_ESP_WIFI_SSID "nepaldigisys"
// #define EXAMPLE_ESP_WIFI_PASS "NDS_0ffice"
#define CONFIG_EXAMPLE_PROV_MGR_MAX_RETRY_CNT 10

#define MB_PORT_NUM 2         // Number of UART port used for Modbus connection
#define MB_DEV_SPEED 9600     // The communication speed of the UART
#define CONFIG_MB_UART_RXD 22 // esp32->22
#define CONFIG_MB_UART_TXD 23 // esp32->23
// #define CONFIG_MB_UART_RXD      16
// #define CONFIG_MB_UART_TXD      17
#define CONFIG_MB_UART_RTS 18 // esp32->18

#define CONFIG_MB_COMM_MODE_RTU 1

// Note: Some pins on target chip cannot be assigned for UART communication.
// See UART documentation for selected board and target to configure pins using Kconfig.

// The number of parameters that intended to be used in the particular control process
#define MASTER_MAX_CIDS num_device_parameters

// Number of reading of parameters from slave
#define MASTER_MAX_RETRY 30

// Timeout to update cid over Modbus
#define UPDATE_CIDS_TIMEOUT_MS (500)
#define UPDATE_CIDS_TIMEOUT_TICS (UPDATE_CIDS_TIMEOUT_MS / portTICK_RATE_MS)

// Timeout between polls
#define POLL_TIMEOUT_MS (500)
#define POLL_TIMEOUT_TICS (POLL_TIMEOUT_MS / portTICK_RATE_MS)

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// The macro to get offset for parameter in the appropriate structure
#define HOLD_OFFSET(field) ((uint16_t)(offsetof(holding_reg_params_t, field) + 1))
#define INPUT_OFFSET(field) ((uint16_t)(offsetof(input_reg_params_t, field) + 1))
#define COIL_OFFSET(field) ((uint16_t)(offsetof(coil_reg_params_t, field) + 1))
// Discrete offset macro
#define DISCR_OFFSET(field) ((uint16_t)(offsetof(discrete_reg_params_t, field) + 1))

#define STR(fieldname) ((const char *)(fieldname))
// Options can be used as bit masks or parameter limits
#define OPTS(min_val, max_val, step_val)                   \
    {                                                      \
        .opt1 = min_val, .opt2 = max_val, .opt3 = step_val \
    }

// this macro is used to specify the delay for 1 sec.
#define APP_DELAY_ONE_SEC 1000u

static int config_publish_period = APP_DELAY_ONE_SEC;

// static float temperature = 25.0;
// static float humidity = 85.0;

static char energymeter_stream[] = "energymeter_stream";

static bytebeam_client_t bytebeam_client;

static const char *TAG = "ENERGYMETER-BYTEBEAM";

static int s_retry_num = 0;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

// Enumeration of modbus device addresses accessed by master device
enum
{
    MB_DEVICE_ADDR1 = 1 // Only one slave device used for the test (add other slave addresses here)
};

// Enumeration of all supported CIDs for device (used in parameter definition table)
enum
{
    CID_MFM384_INP_DATA_V_1 = 0,
    CID_MFM384_INP_DATA_V_2,
    CID_MFM384_INP_DATA_V_3,
    // CID_MFM384_INP_DATA_I1,
    // CID_MFM384_INP_DATA_I2,
    // CID_MFM384_INP_DATA_I3,
    CID_MFM384_INP_DATA_AVG_I,
    CID_MFM384_INP_DATA_FREQUENCY,
    CID_MFM384_INP_DATA_KW,
    CID_MFM384_INP_DATA_KWH,
    CID_MFM384_INP_DATA_PF_AVG, 
    CID_COUNT
};

typedef struct param_energymeter
{
    float voltage_1;
    float voltage_2;
    float voltage_3;
    float current_avg;
    // float current_1;
    // float current_2;
    // float current_3;
    float total_kw;
    float total_kwh;
    float frequencey;
    float avg_pf;
} param_energymeter_t;

param_energymeter_t energyvals;

bool flag_new_modbus_data_available = false;
// Example Data (Object) Dictionary for Modbus parameters:
// The CID field in the table must be unique.
// Modbus Slave Addr field defines slave address of the device with correspond parameter.
// Modbus Reg Type - Type of Modbus register area (Holding register, Input Register and such).
// Reg Start field defines the start Modbus register number and Reg Size defines the number of registers for the characteristic accordingly.
// The Instance Offset defines offset in the appropriate parameter structure that will be used as instance to save parameter value.
// Data Type, Data Size specify type of the characteristic and its data size.
// Parameter Options field specifies the options that can be used to process parameter value (limits or masks).
// Access Mode - can be used to implement custom options for processing of characteristic (Read/Write restrictions, factory mode values and etc).
const mb_parameter_descriptor_t device_parameters[] = {
    // { CID, Param Name, Units, Modbus Slave Addr, Modbus Reg Type, Reg Start, Reg Size, Instance Offset, Data Type, Data Size, Parameter Options, Access Mode}
    {CID_MFM384_INP_DATA_V_1, STR("Voltage 1"), STR("Volts"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0, 2,
     INPUT_OFFSET(volatage_1), PARAM_TYPE_FLOAT, 4, OPTS(-100, 100, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},

    {CID_MFM384_INP_DATA_V_2, STR("Voltage 2"), STR("Volts"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 2, 2,
     INPUT_OFFSET(volatage_2), PARAM_TYPE_FLOAT, 4, OPTS(-100, 100, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},

    {CID_MFM384_INP_DATA_V_3, STR("Voltage 3"), STR("Volts"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 4, 2,
     INPUT_OFFSET(volatage_3), PARAM_TYPE_FLOAT, 4, OPTS(-100, 100, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},          

    // {CID_MFM384_INP_DATA_I1, STR("Current I1"), STR("Amps"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 16, 2,
    //  INPUT_OFFSET(current_1), PARAM_TYPE_FLOAT, 4, OPTS(-100, 100, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},

    // {CID_MFM384_INP_DATA_I2, STR("Current I2"), STR("Amps"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 18, 2,
    //  INPUT_OFFSET(current_2), PARAM_TYPE_FLOAT, 4, OPTS(-100, 100, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},

    // {CID_MFM384_INP_DATA_I3, STR("Current I3"), STR("Amps"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 20, 2,
    //  INPUT_OFFSET(current_3), PARAM_TYPE_FLOAT, 4, OPTS(-100, 100, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},   

    {CID_MFM384_INP_DATA_AVG_I, STR("Current avg"), STR("Amps"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 22, 2,
     INPUT_OFFSET(current_avg), PARAM_TYPE_FLOAT, 4, OPTS(-100, 100, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},   

    {CID_MFM384_INP_DATA_FREQUENCY, STR("Frequency"), STR("Hz"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 56, 2,
     INPUT_OFFSET(frequencey), PARAM_TYPE_FLOAT, 4, OPTS(0, 100, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},

    {CID_MFM384_INP_DATA_KWH, STR("Units"), STR("KWh"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 58, 2,
     INPUT_OFFSET(total_kwh), PARAM_TYPE_FLOAT, 4, OPTS(0, 10000, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},
    
    {CID_MFM384_INP_DATA_KW, STR("KW"), STR("KW"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 42, 2,
     INPUT_OFFSET(total_kw), PARAM_TYPE_FLOAT, 4, OPTS(0, 10000, 0.1), PAR_PERMS_READ_WRITE_TRIGGER},

    {CID_MFM384_INP_DATA_PF_AVG, STR("pf"), STR("none"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 54, 2,
     INPUT_OFFSET(avg_pf), PARAM_TYPE_FLOAT, 4, OPTS(0, 1, 0.001), PAR_PERMS_READ_WRITE_TRIGGER}
     
};

// Calculate number of parameters in the table
const uint16_t num_device_parameters = (sizeof(device_parameters) / sizeof(device_parameters[0]));
// const uint16_t num_device_parameters = 10;

// The function to get pointer to parameter storage (instance) according to parameter description table
static void *master_get_param_data(const mb_parameter_descriptor_t *param_descriptor)
{
    assert(param_descriptor != NULL);
    void *instance_ptr = NULL;
    if (param_descriptor->param_offset != 0)
    {
        switch (param_descriptor->mb_param_type)
        {
        case MB_PARAM_HOLDING:
            //    instance_ptr = ((void*)&holding_reg_params + param_descriptor->param_offset - 1);
            break;
        case MB_PARAM_INPUT:
            instance_ptr = ((void *)&input_reg_params + param_descriptor->param_offset - 1);
            break;
        case MB_PARAM_COIL:
            //    instance_ptr = ((void*)&coil_reg_params + param_descriptor->param_offset - 1);
            break;
        case MB_PARAM_DISCRETE:
            //    instance_ptr = ((void*)&discrete_reg_params + param_descriptor->param_offset - 1);
            break;
        default:
            instance_ptr = NULL;
            break;
        }
    }
    else
    {
        ESP_LOGE(TAG, "Wrong parameter offset for CID #%d", param_descriptor->cid);
        assert(instance_ptr != NULL);
    }
    return instance_ptr;
}


/* Event handler for catching system events */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
    static int retries;
#endif
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "Received Wi-Fi credentials"
                         "\n\tSSID     : %s\n\tPassword : %s",
                         (const char *) wifi_sta_cfg->ssid,
                         (const char *) wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                         "\n\tPlease reset to factory and retry provisioning",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
                retries++;
                if (retries >= CONFIG_EXAMPLE_PROV_MGR_MAX_RETRY_CNT) {
                    ESP_LOGI(TAG, "Failed to connect with provisioned AP, reseting provisioned credentials");
                    wifi_prov_mgr_reset_sm_state_on_failure();
                    retries = 0;
                }
#endif
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
                retries = 0;
#endif
                break;
            case WIFI_PROV_END:
                /* De-initialize manager once provisioning is finished */
                wifi_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        // s_retry_num = 0;
        retries = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        retries++;
        if (retries >= CONFIG_EXAMPLE_PROV_MGR_MAX_RETRY_CNT) {
            ESP_LOGI(TAG, "Failed to connect with provisioned AP, reseting provisioned credentials");
            wifi_prov_mgr_reset_provisioning();
            wifi_prov_mgr_reset_sm_state_on_failure();
            retries = 0;
        }
        esp_wifi_connect();
    }
}

/* Handler for the optional provisioning endpoint registered by the application.
 * The data format can be chosen by applications. Here, we are using plain ascii text.
 * Applications can choose to use other formats like protobuf, JSON, XML, etc.
 */
esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                          uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    if (inbuf) {
        ESP_LOGI(TAG, "Received data: %.*s", inlen, (char *)inbuf);
    }
    char response[] = "SUCCESS";
    *outbuf = (uint8_t *)strdup(response);
    if (*outbuf == NULL) {
        ESP_LOGE(TAG, "System out of memory");
        return ESP_ERR_NO_MEM;
    }
    *outlen = strlen(response) + 1; /* +1 for NULL terminating byte */

    return ESP_OK;
}

// User operation function to read slave values and check alarm
static void modbus_master_operation(void *arg)
{
    esp_err_t err = ESP_OK;
    float value = 0;
    bool alarm_state = false;
    const mb_parameter_descriptor_t *param_descriptor = NULL;

    ESP_LOGI(TAG, "Start modbus test...");

    for (;;)
    {
        // Read all found characteristics from slave(s)
        for (uint16_t cid = 0; (err != ESP_ERR_NOT_FOUND) && cid < MASTER_MAX_CIDS; cid++)
        {
            // Get data from parameters description table
            // and use this information to fill the characteristics description table
            // and having all required fields in just one table
            err = mbc_master_get_cid_info(cid, &param_descriptor);
            if ((err != ESP_ERR_NOT_FOUND) && (param_descriptor != NULL))
            {
                void *temp_data_ptr = master_get_param_data(param_descriptor);
                assert(temp_data_ptr);
                uint8_t type = 0;
                {
                    err = mbc_master_get_parameter(cid, (char *)param_descriptor->param_key,
                                                   (uint8_t *)&value, &type);
                    if (err == ESP_OK)
                    {
                        *(float *)temp_data_ptr = value;
                        if ((param_descriptor->mb_param_type == MB_PARAM_HOLDING) ||
                            (param_descriptor->mb_param_type == MB_PARAM_INPUT))
                        {
                            ESP_LOGI(TAG, "Characteristic #%d %s (%s) value = %f (0x%x) read successful.",
                                     param_descriptor->cid,
                                     (char *)param_descriptor->param_key,
                                     (char *)param_descriptor->param_units,
                                     value,
                                     *(uint32_t *)temp_data_ptr);
                            // if (((value > param_descriptor->param_opts.max) ||
                            //     (value < param_descriptor->param_opts.min))) {
                            //         alarm_state = true;
                            //         break;
                            // }
                            switch (param_descriptor->cid)
                            {
                            case CID_MFM384_INP_DATA_V_1:
                                energyvals.voltage_1 = value;
                                break;
                            case CID_MFM384_INP_DATA_V_2:
                                energyvals.voltage_2 = value;
                                break;    
                            case CID_MFM384_INP_DATA_V_3:
                                energyvals.voltage_3 = value;
                                break;                                                                
                            case CID_MFM384_INP_DATA_AVG_I:
                                energyvals.current_avg = value;
                                break;                                
                            // case CID_MFM384_INP_DATA_I1:
                            //     energyvals.current_1 = value;
                            //     break;
                            // case CID_MFM384_INP_DATA_I2:
                            //     energyvals.current_2 = value;
                            //     break;
                            // case CID_MFM384_INP_DATA_I3:
                            //     energyvals.current_3 = value;
                            //     break;                                                                
                            case CID_MFM384_INP_DATA_KW:
                                energyvals.total_kw = value;
                                break;
                            case CID_MFM384_INP_DATA_KWH:
                                energyvals.total_kwh = value;
                                break;                            
                            case CID_MFM384_INP_DATA_FREQUENCY:
                                energyvals.frequencey = value;
                                break;
                            case CID_MFM384_INP_DATA_PF_AVG:
                                energyvals.avg_pf = value;
                                break;                                
                            default:
                                break;
                                flag_new_modbus_data_available = true;
                            }
                        }
                        else
                        {
                            uint16_t state = *(uint16_t *)temp_data_ptr;
                            const char *rw_str = (state & param_descriptor->param_opts.opt1) ? "ON" : "OFF";
                            ESP_LOGI(TAG, "Characteristic #%d %s (%s) value = %s (0x%x) read successful.",
                                     param_descriptor->cid,
                                     (char *)param_descriptor->param_key,
                                     (char *)param_descriptor->param_units,
                                     (const char *)rw_str,
                                     *(uint16_t *)temp_data_ptr);
                            if (state & param_descriptor->param_opts.opt1)
                            {
                                alarm_state = true;
                                break;
                            }
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Characteristic #%d (%s) read fail, err = 0x%x (%s).",
                                 param_descriptor->cid,
                                 (char *)param_descriptor->param_key,
                                 (int)err,
                                 (char *)esp_err_to_name(err));
                    }
                }
                vTaskDelay(POLL_TIMEOUT_TICS); // timeout between polls
            }
        }
        vTaskDelay(UPDATE_CIDS_TIMEOUT_TICS); //
    }

    if (alarm_state)
    {
        ESP_LOGI(TAG, "Alarm triggered by cid #%d.",
                 param_descriptor->cid);
    }
    else
    {
        ESP_LOGE(TAG, "Alarm is not triggered after %d retries.",
                 MASTER_MAX_RETRY);
    }
    ESP_LOGI(TAG, "Destroy master...");
    ESP_ERROR_CHECK(mbc_master_destroy());
}

// Modbus master initialization
static esp_err_t master_init(void)
{
    // Initialize and start Modbus controller
    mb_communication_info_t comm = {
        .port = MB_PORT_NUM,
#if CONFIG_MB_COMM_MODE_ASCII
        .mode = MB_MODE_ASCII,
#elif CONFIG_MB_COMM_MODE_RTU
        .mode = MB_MODE_RTU,
#endif
        .baudrate = MB_DEV_SPEED,
        .parity = MB_PARITY_NONE
    };
    void *master_handler = NULL;

    esp_err_t err = mbc_master_init(MB_PORT_SERIAL_MASTER, &master_handler);
    MB_RETURN_ON_FALSE((master_handler != NULL), ESP_ERR_INVALID_STATE, TAG,
                       "mb controller initialization fail.");
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb controller initialization fail, returns(0x%x).",
                       (uint32_t)err);
    err = mbc_master_setup((void *)&comm);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb controller setup fail, returns(0x%x).",
                       (uint32_t)err);

    // Set UART pin numbers
    err = uart_set_pin(MB_PORT_NUM, CONFIG_MB_UART_TXD, CONFIG_MB_UART_RXD,
                       CONFIG_MB_UART_RTS, UART_PIN_NO_CHANGE);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb serial set pin failure, uart_set_pin() returned (0x%x).", (uint32_t)err);

    err = mbc_master_start();
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb controller start fail, returns(0x%x).",
                       (uint32_t)err);

    // Set driver mode to Half Duplex
    err = uart_set_mode(MB_PORT_NUM, UART_MODE_RS485_HALF_DUPLEX);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb serial set mode failure, uart_set_mode() returned (0x%x).", (uint32_t)err);

    vTaskDelay(5);
    err = mbc_master_set_descriptor(&device_parameters[0], num_device_parameters);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb controller set descriptor fail, returns(0x%x).",
                       (uint32_t)err);
    ESP_LOGI(TAG, "Modbus master stack initialized...");
    return err;
}

static void wifi_init_sta(void)
{
    /* Start Wi-Fi in station mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

#if 0
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

#endif


#if 0
void wifi_init_sta(void)
{
#if 1
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    // ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
#endif

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

#endif

static int publish_energymeter_values(bytebeam_client_t *bytebeam_client)
{
    struct timeval te;
    long long milliseconds = 0;
    static uint64_t sequence = 0;

    cJSON *device_shadow_json_list = NULL;
    cJSON *device_shadow_json = NULL;
    cJSON *sequence_json = NULL;
    cJSON *timestamp_json = NULL;
    cJSON *device_status_json = NULL;
    cJSON *voltage_1_json = NULL;
    cJSON *voltage_2_json = NULL;
    cJSON *voltage_3_json = NULL;
    // cJSON *current_1_json = NULL;
    // cJSON *current_2_json = NULL;
    // cJSON *current_3_json = NULL;
    cJSON *current_avg_json  = NULL;
    cJSON *totalkw_json = NULL;
    cJSON *totalkwh_json = NULL;
    cJSON *frequency_json = NULL;
    cJSON *avg_pf_json = NULL;    

    char *string_json = NULL;

    device_shadow_json_list = cJSON_CreateArray();

    if (device_shadow_json_list == NULL)
    {
        ESP_LOGE(TAG, "Json Init failed.");
        return -1;
    }

    device_shadow_json = cJSON_CreateObject();

    if (device_shadow_json == NULL)
    {
        ESP_LOGE(TAG, "Json add failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    // get current time
    gettimeofday(&te, NULL);
    milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000;

    timestamp_json = cJSON_CreateNumber(milliseconds);

    if (timestamp_json == NULL)
    {
        ESP_LOGE(TAG, "Json add time stamp failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    cJSON_AddItemToObject(device_shadow_json, "timestamp", timestamp_json);

    sequence++;
    sequence_json = cJSON_CreateNumber(sequence);

    if (sequence_json == NULL)
    {
        ESP_LOGE(TAG, "Json add sequence id failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    cJSON_AddItemToObject(device_shadow_json, "sequence", sequence_json);

    // Add voltage
    // voltage_v1_json = cJSON_CreateNumber(energyvals.volatage);

    // if (voltage_v1_json == NULL)
    // {
    //     ESP_LOGE(TAG, "Json add voltage failed.");
    //     cJSON_Delete(device_shadow_json_list);
    //     return -1;
    // }
    // cJSON_AddItemToObject(device_shadow_json, "voltage", voltage_v1_json);


    // // Add voltage 2
    // voltage_2_json = cJSON_CreateNumber(energyvals.volatage_2);

    // if (voltage_2_json == NULL)
    // {
    //     ESP_LOGE(TAG, "Json add voltage failed.");
    //     cJSON_Delete(device_shadow_json_list);
    //     return -1;
    // }
    // cJSON_AddItemToObject(device_shadow_json, "voltage", voltage_2_json);

    // Add Voltage 1
    voltage_1_json = cJSON_CreateNumber(energyvals.voltage_1);
    if (voltage_1_json == NULL)
    {
        ESP_LOGE(TAG, "Json add voltage 1 failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    cJSON_AddItemToObject(device_shadow_json, "voltage_1", voltage_1_json);

    // Add Voltage 2
    voltage_2_json = cJSON_CreateNumber(energyvals.voltage_2);
    if (voltage_2_json == NULL)
    {
        ESP_LOGE(TAG, "Json add voltage 2 failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    cJSON_AddItemToObject(device_shadow_json, "voltage_2", voltage_2_json);

    // Add Voltage 3
    voltage_3_json = cJSON_CreateNumber(energyvals.voltage_3);
    if (voltage_3_json == NULL)
    {
        ESP_LOGE(TAG, "Json add voltage 3 failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    cJSON_AddItemToObject(device_shadow_json, "voltage_3", voltage_3_json);

    // Add Avg I
    current_avg_json = cJSON_CreateNumber(energyvals.voltage_3);
    if (current_avg_json == NULL)
    {
        ESP_LOGE(TAG, "Json add voltage 3 failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    cJSON_AddItemToObject(device_shadow_json, "avg_current", current_avg_json);

#if 0
    // Add Current 1
    current_1_json = cJSON_CreateNumber(energyvals.current_1);
    if (current_1_json == NULL)
    {
        ESP_LOGE(TAG, "Json add current 1 failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    cJSON_AddItemToObject(device_shadow_json, "current_1", current_1_json);

    // Add Current 2
    current_2_json = cJSON_CreateNumber(energyvals.current_2);
    if (current_2_json == NULL)
    {
        ESP_LOGE(TAG, "Json add current 2 failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    cJSON_AddItemToObject(device_shadow_json, "current_2", current_2_json);

    // Add Current 3
    current_3_json = cJSON_CreateNumber(energyvals.current_3);
    if (current_3_json == NULL)
    {
        ESP_LOGE(TAG, "Json add current 3 failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    cJSON_AddItemToObject(device_shadow_json, "current_3", current_3_json);
#endif 

    // Add Current
    // current_i1_json = cJSON_CreateNumber(energyvals.current);

    // if (current_i1_json == NULL)
    // {
    //     ESP_LOGE(TAG, "Json add Current failed.");
    //     cJSON_Delete(device_shadow_json_list);
    //     return -1;
    // }
    // cJSON_AddItemToObject(device_shadow_json, "current", current_i1_json);


    // Add total KW
    totalkw_json = cJSON_CreateNumber(energyvals.total_kw);

    if (totalkwh_json == NULL)
    {
        ESP_LOGE(TAG, "Json add voltage failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    cJSON_AddItemToObject(device_shadow_json, "totalkw", totalkw_json);

    // Add total KWh
    totalkwh_json = cJSON_CreateNumber(energyvals.total_kwh);

    if (totalkwh_json == NULL)
    {
        ESP_LOGE(TAG, "Json add voltage failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    cJSON_AddItemToObject(device_shadow_json, "totalkwh", totalkwh_json);

    // Add frequency
    frequency_json = cJSON_CreateNumber(energyvals.frequencey);

    if (frequency_json == NULL)
    {
        ESP_LOGE(TAG, "Json add humidity failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    cJSON_AddItemToObject(device_shadow_json, "frequency", frequency_json);

    // Add pf
    avg_pf_json = cJSON_CreateNumber(energyvals.avg_pf);

    if (avg_pf_json == NULL)
    {
        ESP_LOGE(TAG, "Json add humidity failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }
    cJSON_AddItemToObject(device_shadow_json, "avg_pf", avg_pf_json);
    
    cJSON_AddItemToArray(device_shadow_json_list, device_shadow_json);

    string_json = cJSON_Print(device_shadow_json_list);

    if (string_json == NULL)
    {
        ESP_LOGE(TAG, "Json string print failed.");
        cJSON_Delete(device_shadow_json_list);
        return -1;
    }

    ESP_LOGI(TAG, "\nStatus to send:\n%s\n", string_json);

    // int ret_val = 0;
    // if(flag_new_modbus_data_available) {
    // flag_new_modbus_data_available = false;
    // publish the json to sht stream
    int ret_val = bytebeam_publish_to_stream(bytebeam_client, energymeter_stream, string_json);
    // } else {
    //     ESP_LOGE(TAG, "Could not get new modbus reading");
    // }

    cJSON_Delete(device_shadow_json_list);
    cJSON_free(string_json);

    return ret_val;
}

static void app_start(bytebeam_client_t *bytebeam_client)
{
    int ret_val = 0;

    while (1)
    {
        // publish sht values
        ret_val = publish_energymeter_values(bytebeam_client);

        if (ret_val != 0)
        {
            ESP_LOGE(TAG, "Failed to publish energymeter values.");
        }

        vTaskDelay(config_publish_period / portTICK_PERIOD_MS);
    }
}

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);

#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif

    sntp_init();
}

static void sync_time_from_ntp(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;

    initialize_sntp();

    // wait for time to be set
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    time(&now);
    localtime_r(&now, &timeinfo);
}

void app_main(void)
{
    // Initialization of device peripheral and objects
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    #if 0
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    vTaskDelay(1000);
    #endif


    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated
         * and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());

        /* Retry nvs_flash_init */
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Initialize TCP/IP */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize the event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();

    /* Register our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    /* Initialize Wi-Fi including netif with default config */
    esp_netif_create_default_wifi_sta();
#ifdef CONFIG_EXAMPLE_PROV_TRANSPORT_SOFTAP
    esp_netif_create_default_wifi_ap();
#endif /* CONFIG_EXAMPLE_PROV_TRANSPORT_SOFTAP */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {
        /* What is the Provisioning Scheme that we want ?
         * wifi_prov_scheme_softap or wifi_prov_scheme_ble */
#ifdef CONFIG_EXAMPLE_PROV_TRANSPORT_SOFTAP
        .scheme = wifi_prov_scheme_softap,
#endif /* CONFIG_EXAMPLE_PROV_TRANSPORT_SOFTAP */

        /* Any default scheme specific event handler that you would
         * like to choose. Since our example application requires
         * neither BT nor BLE, we can choose to release the associated
         * memory once provisioning is complete, or not needed
         * (in case when device is already provisioned). Choosing
         * appropriate scheme specific event handler allows the manager
         * to take care of this automatically. This can be set to
         * WIFI_PROV_EVENT_HANDLER_NONE when using wifi_prov_scheme_softap*/
#ifdef CONFIG_EXAMPLE_PROV_TRANSPORT_SOFTAP
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
#endif /* CONFIG_EXAMPLE_PROV_TRANSPORT_SOFTAP */
    };

    /* Initialize provisioning manager with the
     * configuration parameters set above */
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    bool provisioned = false;
#ifdef CONFIG_EXAMPLE_RESET_PROVISIONED
    wifi_prov_mgr_reset_provisioning();
#else
    /* Let's find out if the device is provisioned */
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

#endif
    /* If device is not yet provisioned start provisioning service */
    if (!provisioned) {
        ESP_LOGI(TAG, "Starting provisioning");

        /* What is the Device Service Name that we want
         * This translates to :
         *     - Wi-Fi SSID when scheme is wifi_prov_scheme_softap
         *     - device name when scheme is wifi_prov_scheme_ble
         */
        char service_name[12] = "PROV_ENRG";
        // get_device_service_name(service_name, sizeof(service_name));

        /* What is the security level that we want (0 or 1):
         *      - WIFI_PROV_SECURITY_0 is simply plain text communication.
         *      - WIFI_PROV_SECURITY_1 is secure communication which consists of secure handshake
         *          using X25519 key exchange and proof of possession (pop) and AES-CTR
         *          for encryption/decryption of messages.
         */
        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;

        /* Do we want a proof-of-possession (ignored if Security 0 is selected):
         *      - this should be a string with length > 0
         *      - NULL if not used
         */
        const char *pop = "abcd1234";

        /* What is the service key (could be NULL)
         * This translates to :
         *     - Wi-Fi password when scheme is wifi_prov_scheme_softap
         *          (Minimum expected length: 8, maximum 64 for WPA2-PSK)
         *     - simply ignored when scheme is wifi_prov_scheme_ble
         */
        const char *service_key = NULL;

        /* An optional endpoint that applications can create if they expect to
         * get some additional custom data during provisioning workflow.
         * The endpoint name can be anything of your choice.
         * This call must be made before starting the provisioning.
         */
        wifi_prov_mgr_endpoint_create("custom-data");
        /* Start provisioning service */
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));

        /* The handler for the optional endpoint created above.
         * This call must be made after starting the provisioning, and only if the endpoint
         * has already been created above.
         */
        wifi_prov_mgr_endpoint_register("custom-data", custom_prov_data_handler, NULL);

        /* Uncomment the following to wait for the provisioning to finish and then release
         * the resources of the manager. Since in this case de-initialization is triggered
         * by the default event loop handler, we don't need to call the following */
        // wifi_prov_mgr_wait();
        // wifi_prov_mgr_deinit();
        /* Print QR code for provisioning */
        // wifi_prov_print_qr(service_name, pop, PROV_TRANSPORT_SOFTAP);

    } else {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

        /* We don't need the manager as device is already provisioned,
         * so let's release it's resources */
        wifi_prov_mgr_deinit();

        /* Start Wi-Fi station */
        wifi_init_sta();
    }

    /* Wait for Wi-Fi connection */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, portMAX_DELAY);    

    ESP_ERROR_CHECK(master_init());
    // master_init();
    vTaskDelay(10);

    // sync time from the ntp
    sync_time_from_ntp();

    // initialize the bytebeam client
    bytebeam_init(&bytebeam_client);

    xTaskCreate(modbus_master_operation, "Modbus Master", 2 * 2048, NULL, tskIDLE_PRIORITY, NULL);

    // start the bytebeam client
    bytebeam_start(&bytebeam_client);

    //
    // start the main application
    //
    app_start(&bytebeam_client);
}
