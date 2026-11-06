/*
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023. All rights reserved.
 * Description: sle uuid server sample.
 */
#include "app_init.h"
#include "watchdog.h"
#include "tcxo.h"
#include "systick.h"
#include "los_memory.h"
#include "securec.h"
#include "errcode.h"
#include "osal_addr.h"
#include "soc_osal.h"
#include "common_def.h"
#include "nv.h"
#include "sle_common.h"
#include "sle_errcode.h"
#include "sle_ssap_server.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_tx_adv.h"
#include "sle_transmition_manager.h"
#include "sle_tx.h"
#include "hy_sensor.h"      /* 手表传感器状态 g_hy（心率/血氧/步数/跌倒） */

#define OCTET_BIT_LEN 8
#define UUID_LEN_2     2
#define BT_INDEX_4     4
#define BT_INDEX_5     5
#define BT_INDEX_0     0
extern void send_data_thread_function(void);
#define encode2byte_little(_ptr, data) \
    do { \
        *(uint8_t *)((_ptr) + 1) = (uint8_t)((data) >> 8); \
        *(uint8_t *)(_ptr) = (uint8_t)(data); \
    } while (0)

/* sle server app uuid for test */
static char g_sle_uuid_app_uuid[UUID_LEN_2] = {0x0, 0x0};
/* server notify property uuid for test */
static char g_sle_property_value[OCTET_BIT_LEN] = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
/* sle connect acb handle */
static uint16_t g_sle_conn_hdl = 0;
/* sle server handle */
static uint8_t g_server_id = 0;
/* sle service handle */
static uint16_t g_service_handle = 0;
/* sle ntf property handle */
static uint16_t g_property_handle = 0;
osal_task *g_task_handle = NULL;
#ifdef SLE_QOS_FLOWCTRL_FUNCTION_SWITCH
static sle_link_qos_state_t g_sle_link_state = 0;  /* sle link state */
#endif

#ifdef CONFIG_LARGE_THROUGHPUT_SERVER
#define PKT_DATA_LEN 1200
#define SPEED_DEFAULT_CONN_INTERVAL 0x14
#else
#define PKT_DATA_LEN 600
#define SPEED_DEFAULT_CONN_INTERVAL 0xA0
#endif

#define SPEED_DEFAULT_KTHREAD_SIZE 0x2000
#define SPEED_DEFAULT_KTHREAD_PROI 26
#define DEFAULT_SLE_SPEED_DATA_LEN 1500
#define DEFAULT_SLE_SPEED_MTU_SIZE 1500
#define SPEED_DEFAULT_TIMEOUT_MULTIPLIER 0x1f4
#define DEFAULT_SLE_SPEED_MCS 10
#define WAIT_MCS_SET 2000
static unsigned char data[PKT_DATA_LEN];

static uint8_t sle_uuid_base[] = { 0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA, \
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static void sle_uuid_set_base(sle_uuid_t *out)
{
    (void)memcpy_s(out->uuid, SLE_UUID_LEN, sle_uuid_base, SLE_UUID_LEN);
    out->len = UUID_LEN_2;
}

static void sle_uuid_setu2(uint16_t u2, sle_uuid_t *out)
{
    sle_uuid_set_base(out);
    out->len = UUID_LEN_2;
    encode2byte_little(&out->uuid[14], u2);
}

static void destroy_send_data_thread(void)
{
    if (g_task_handle != NULL) {
        osal_kthread_destroy(g_task_handle, 1);
        osal_kfree(g_task_handle);
        g_task_handle = NULL;
    }
}

static void ssaps_read_request_cbk(uint8_t server_id, uint16_t conn_id, ssaps_req_read_cb_t *read_cb_para,
    errcode_t status)
{
    osal_printk("[speed server] ssaps read request cbk server_id:0x%x, conn_id:0x%x, handle:0x%x, status:0x%x\r\n",
        server_id, conn_id, read_cb_para->handle, status);
    // 每次创建发包线程前检查线程是否已经创建，如果已经创建，则销毁当前线程。server端只允许有一个发包线程存在。
    destroy_send_data_thread();
    osal_kthread_lock();
    g_task_handle = osal_kthread_create((osal_kthread_handler)send_data_thread_function,
        0, "ThroughputTask", SPEED_DEFAULT_KTHREAD_SIZE);
    if (g_task_handle != NULL) {
        osal_kthread_set_priority(g_task_handle, SPEED_DEFAULT_KTHREAD_PROI + 1);
    }
    osal_kthread_unlock();
    printf("kthread success\r\n");
}

static void ssaps_write_request_cbk(uint8_t server_id, uint16_t conn_id, ssaps_req_write_cb_t *write_cb_para,
    errcode_t status)
{
    osal_printk("[speed server] ssaps write request cbk server_id:%d, conn_id:%d, handle:%d, status:%d\r\n",
        server_id, conn_id, write_cb_para->handle, status);

    /* ---- 新增：把对端写过来的业务数据原样打出来 ----
     * 星闪链路上收到的是一条 JSON 字符串，这里只看内容和长度。 */
    if (write_cb_para->value != NULL && write_cb_para->length > 0) {
        char txt[81] = {0};
        uint16_t n = write_cb_para->length;
        uint16_t i;

        if (n > 80) {
            n = 80;
        }
        for (i = 0; i < n; i++) {
            uint8_t c = write_cb_para->value[i];
            txt[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        osal_printk("[SLE RX] len=%d text=%s\r\n", write_cb_para->length, txt);
    }
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id,  ssap_exchange_info_t *mtu_size,
    errcode_t status)
{
    osal_printk("[speed server] ssaps write request cbk server_id:%d, conn_id:%d, mtu_size:%d, status:%d\r\n",
        server_id, conn_id, mtu_size->mtu_size, status);
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    osal_printk("[speed server] start service cbk server_id:%d, handle:%d, status:%d\r\n",
        server_id, handle, status);
}

static void sle_ssaps_register_cbks(void)
{
    ssaps_callbacks_t ssaps_cbk = {0};
    ssaps_cbk.start_service_cb = ssaps_start_service_cbk;
    ssaps_cbk.mtu_changed_cb = ssaps_mtu_changed_cbk;
    ssaps_cbk.read_request_cb = ssaps_read_request_cbk;
    ssaps_cbk.write_request_cb = ssaps_write_request_cbk;
    ssaps_register_callbacks(&ssaps_cbk);
}

errcode_t sle_uuid_server_send_report_by_handle_id(uint8_t *data, uint16_t len, uint16_t connect_id)
{
    ssaps_ntf_ind_t param = {0};
    param.handle = g_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value = data;
    param.value_len = len;
    ssaps_notify_indicate(g_server_id, connect_id, &param);
    return ERRCODE_SLE_SUCCESS;
}

#ifdef SLE_QOS_FLOWCTRL_FUNCTION_SWITCH
static void sle_send_data_cbk(uint16_t conn_id, sle_link_qos_state_t link_state)
{
    conn_id = conn_id;
    g_sle_link_state = link_state;
    printf("%s enter, gle_tx_acb_data_num_get:%d.\n", __FUNCTION__, link_state);
}

static void sle_transmission_register_cbks(void)
{
    sle_transmission_callbacks_t trans_cbk = {0};
    trans_cbk.send_data_cb = sle_send_data_cbk;
    sle_transmission_register_callbacks(&trans_cbk);
}
#else
extern uint8_t gle_tx_acb_data_num_get(void);
#endif

uint8_t sle_flow_ctrl_flag(void)
{
#ifdef SLE_QOS_FLOWCTRL_FUNCTION_SWITCH
    return (g_sle_link_state <= SLE_QOS_FLOWCTRL) ? 1 : 0;
#else
    return gle_tx_acb_data_num_get();
#endif
}

void send_data_thread_function(void)
{
    sle_set_data_len(g_sle_conn_hdl, DEFAULT_SLE_SPEED_DATA_LEN);
#ifdef CONFIG_LARGE_THROUGHPUT_SERVER
    sle_set_phy_t phy_parm = {
        .tx_format = SLE_RADIO_FRAME_2,
        .rx_format = SLE_RADIO_FRAME_2,
        .tx_phy = SLE_PHY_4M,
        .rx_phy = SLE_PHY_4M,
        .tx_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1,
        .rx_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1,
        .g_feedback = 0,
        .t_feedback = 0,
    };
    sle_set_phy_param(g_sle_conn_hdl, &phy_parm);
    osal_printk("code: ploar MCS10, PHY 4MHZ, power: 20dbm \r\n");
#else
    osal_printk("code: GFSK, PHY 1MHZ, power: 20dbm \r\n");
#endif
    osal_msleep(WAIT_MCS_SET);

    /* ---- 改造：不再满速灌数据，改成每秒发一条 JSON ----
     * 角色已调换：本板（手表）当 server 主动上报，主板当 client 收。
     * 数据来自真实传感器 g_hy（hy_sensor_task 在另一个线程里以 199Hz 采样）。 */
    uint32_t seq = 0;
    char txt[96];
    while (1) {
        int n;

        seq++;
        n = snprintf(txt, sizeof(txt),
                     "{\"n\":%u,\"hr\":%d,\"spo2\":%d,\"steps\":%d,\"fall\":%d}",
                     (unsigned)seq, g_hy.hr, g_hy.spo2, g_hy.steps, g_hy.fall_state);
        if (n > 0) {
            (void)sle_uuid_server_send_report_by_handle_id((uint8_t *)txt, (uint16_t)n, g_sle_conn_hdl);
            osal_printk("[SLE TX] %s\r\n", txt);
        }
        osal_msleep(1000);
    }
}

static errcode_t sle_uuid_server_service_add(void)
{
    errcode_t ret;
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_UUID_SERVER_SERVICE, &service_uuid);
    ret = ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[speed server] sle uuid add service fail, ret:0x%x\r\n", ret);
        return ERRCODE_SLE_FAIL;
    }
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uuid_server_property_add(void)
{
    errcode_t ret;
    ssaps_property_info_t property = {0};
    ssaps_desc_info_t descriptor = {0};
    uint8_t ntf_value[] = {0x01, 0x0};

    property.permissions = SLE_UUID_TEST_PROPERTIES;
    sle_uuid_setu2(SLE_UUID_SERVER_NTF_REPORT, &property.uuid);
    property.value = osal_vmalloc(sizeof(g_sle_property_value));
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_NOTIFY;
    if (property.value == NULL) {
        osal_printk("[speed server] sle property mem fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (memcpy_s(property.value, sizeof(g_sle_property_value), g_sle_property_value,
        sizeof(g_sle_property_value)) != EOK) {
        osal_vfree(property.value);
        osal_printk("[speed server] sle property mem cpy fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    ret = ssaps_add_property_sync(g_server_id, g_service_handle, &property,  &g_property_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[speed server] sle uuid add property fail, ret:0x%x\r\n", ret);
        osal_vfree(property.value);
        return ERRCODE_SLE_FAIL;
    }
    descriptor.permissions = SLE_UUID_TEST_DESCRIPTOR;
    descriptor.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    descriptor.type = SSAP_DESCRIPTOR_USER_DESCRIPTION;
    descriptor.value = ntf_value;
    descriptor.value_len = sizeof(ntf_value);

    ret = ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_property_handle, &descriptor);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[speed server] sle uuid add descriptor fail, ret:0x%x\r\n", ret);
        osal_vfree(property.value);
        return ERRCODE_SLE_FAIL;
    }
    osal_vfree(property.value);
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uuid_server_add(void)
{
    errcode_t ret;
    sle_uuid_t app_uuid = {0};

    osal_printk("[speed server] sle uuid add service in\r\n");
    app_uuid.len = sizeof(g_sle_uuid_app_uuid);
    if (memcpy_s(app_uuid.uuid, app_uuid.len, g_sle_uuid_app_uuid, sizeof(g_sle_uuid_app_uuid)) != EOK) {
        return ERRCODE_SLE_FAIL;
    }
    ssaps_register_server(&app_uuid, &g_server_id);

    if (sle_uuid_server_service_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }

    if (sle_uuid_server_property_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }
    osal_printk("[speed server] sle uuid add service, server_id:0x%x, service_handle:0x%x, property_handle:0x%x\r\n",
        g_server_id, g_service_handle, g_property_handle);
    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[speed server] sle uuid add service fail, ret:0x%x\r\n", ret);
        return ERRCODE_SLE_FAIL;
    }
    osal_printk("[speed server] sle uuid add service out\r\n");
    return ERRCODE_SLE_SUCCESS;
}

static void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    osal_printk("[speed server] connect state changed conn_id:0x%02x, conn_state:0x%x, pair_state:0x%x, \
        disc_reason:0x%x\r\n", conn_id, conn_state, pair_state, disc_reason);
    osal_printk("[speed server] connect state changed addr:0x%02x:**:**:**:0x%02x:0x%02x\r\n",
        addr->addr[BT_INDEX_0], addr->addr[BT_INDEX_4], addr->addr[BT_INDEX_5]);
    g_sle_conn_hdl = conn_id;
    sle_connection_param_update_t parame = {0};
    parame.conn_id = conn_id;
    parame.interval_min = SPEED_DEFAULT_CONN_INTERVAL;
    parame.interval_max = SPEED_DEFAULT_CONN_INTERVAL;
    parame.max_latency = 0;
    parame.supervision_timeout = SPEED_DEFAULT_TIMEOUT_MULTIPLIER;
    if (conn_state ==  SLE_ACB_STATE_CONNECTED) {
        sle_update_connect_param(&parame);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        destroy_send_data_thread();
        sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    }
}

static void sle_auth_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status,
    const sle_auth_info_evt_t* evt)
{
    unused(conn_id);
    unused(evt);
    osal_printk("[speed server] auth cmp:0x%x\r\n", status);
    if (status == ERRCODE_SLE_SUCCESS) {
        return;
    }
    osal_printk("[speed server] auth failed, remove pair and restart adv\r\n");
    sle_remove_paired_remote_device(addr);
    sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    osal_printk("[speed server] pair complete conn_id:0x%02x, status:0x%x\r\n",
        conn_id, status);
    osal_printk("[speed server] pair complete addr:0x%02x:**:**:**:0x%02x:0x%02x\r\n",
        addr->addr[BT_INDEX_0], addr->addr[BT_INDEX_4], addr->addr[BT_INDEX_5]);
    if (status == ERRCODE_SLE_SUCCESS) {
        return;
    }
    osal_printk("[speed server] pair failed, remove pair and restart adv\r\n");
    sle_remove_paired_remote_device(addr);
    sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
}

void sle_sample_update_cbk(uint16_t conn_id, errcode_t status, const sle_connection_param_update_evt_t *param)
{
    unused(status);
    osal_printk("[ssap server] updat state changed conn_id:%d, interval = 0x%02x\n", conn_id, param->interval);
}

void sle_sample_update_req_cbk(uint16_t conn_id, errcode_t status, const sle_connection_param_update_req_t *param)
{
    unused(conn_id);
    unused(status);
    osal_printk("[ssap server] sle_sample_update_req_cbk interval_min:0x%02x, interval_max:0x%02x\n",
        param->interval_min, param->interval_max);
}

void sle_sample_rssi_cbk(uint16_t conn_id, int8_t rssi, errcode_t status)
{
    osal_printk("[ssap server] conn_id:%d, rssi = %d, status = 0x%x\n", conn_id, rssi, status);
}

#ifdef CONFIG_LARGE_THROUGHPUT_SERVER
void sle_set_phy_cbk(uint16_t conn_id, errcode_t status, const sle_set_phy_t *param)
{
    osal_printk("sle_set_phy_cbk: handle:%d, status 0x%x, txphy %d, rxphy %d\r\n",
        conn_id,
        status,
        param->tx_phy,
        param->rx_phy);
    sle_set_mcs(g_sle_conn_hdl, DEFAULT_SLE_SPEED_MCS);
}
#endif

static void sle_conn_register_cbks(void)
{
    sle_connection_callbacks_t conn_cbks = {0};
    conn_cbks.connect_state_changed_cb = sle_connect_state_changed_cbk;
    conn_cbks.auth_complete_cb = sle_auth_complete_cbk;
    conn_cbks.pair_complete_cb = sle_pair_complete_cbk;
    conn_cbks.connect_param_update_req_cb = sle_sample_update_req_cbk;
    conn_cbks.connect_param_update_cb = sle_sample_update_cbk;
    conn_cbks.read_rssi_cb = sle_sample_rssi_cbk;
#ifdef CONFIG_LARGE_THROUGHPUT_SERVER
    conn_cbks.set_phy_cb = sle_set_phy_cbk;
#endif
    sle_connection_register_callbacks(&conn_cbks);
}

void sle_ssaps_set_info(void)
{
    ssap_exchange_info_t info = {0};
    info.mtu_size = DEFAULT_SLE_SPEED_MTU_SIZE;
    info.version = 1;
    ssaps_set_info(g_server_id, &info);
}

void sle_set_local_addr_init(void)
{
    sle_addr_t addr = {0};
    uint8_t mac[SLE_ADDR_LEN] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    addr.type = 0;
    memcpy_s(addr.addr, SLE_ADDR_LEN, mac, SLE_ADDR_LEN);
    sle_set_local_addr(&addr);
}

void sle_tx_set_nv(void)
{
    uint16_t nv_value_len = 0;
    uint8_t nv_value = 0;
    uapi_nv_read(0x20A0, sizeof(uint16_t), &nv_value_len, &nv_value);
    if (nv_value != 7) {     // 7:btc功率档位
        nv_value = 7;       // 7:btc功率档位
        uapi_nv_write(0x20A0, (uint8_t *)&(nv_value), sizeof(nv_value));
    }
    osal_printk("[speed server] The value of nv is set to %d.\r\n", nv_value);
}

void sle_enable_server_cbk(void)
{
    sle_tx_set_nv();
    sle_uuid_server_add();
    sle_ssaps_set_info();
#ifdef SLE_QOS_FLOWCTRL_FUNCTION_SWITCH
    sle_transmission_register_cbks();
#endif
    sle_set_local_addr_init();
    sle_uuid_server_adv_init();
    osal_printk("[speed server] init ok\r\n");
}

/* 初始化speed server */
errcode_t sle_tx_init(void)
{
    uapi_watchdog_disable();
    sle_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();
    enable_sle();
    printf("sle enable end.\r\n");
    return ERRCODE_SLE_SUCCESS;
}

int sle_speed_init(void)
{
    for (int i = 0; i < PKT_DATA_LEN; i++) {
        data[i] = 'A';
        data[PKT_DATA_LEN - 1] = '\0';
    }
    osal_msleep(1000);  /* sleep 1000ms */
    sle_tx_init();
    return 0;
}

void sle_tx_start(void)
{
    osal_task *task_handle1 = NULL;

    osal_kthread_lock();
    task_handle1= osal_kthread_create((osal_kthread_handler)sle_speed_init, 0, "speed", SPEED_DEFAULT_KTHREAD_SIZE);
    if (task_handle1 != NULL) {
        osal_kthread_set_priority(task_handle1, SPEED_DEFAULT_KTHREAD_PROI);
        osal_kfree(task_handle1);
    }

    /* 注意：传感器线程由 hy_sensor_main.c 统一创建，这里绝不重复创建，
     * 否则会出现两个 199Hz 采样任务同时写 g_hy。 */
    osal_kthread_unlock();
}

/* Run the blinky_entry. */
/* app_run 交给主工程（hy_sensor_main.c）统一注册 */

