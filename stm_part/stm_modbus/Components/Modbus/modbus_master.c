#include "modbus_master.h"

#include "rs485.h"

#if (MB_REG_COUNT != 2U)
#error "mb_parse() 是按「2 个寄存器 = 温度x10 + 湿度x10」写的，改了 MB_REG_COUNT 要同步改它"
#endif

/*----------------------------------------------- 内部状态 -------------------------------------------*/

static const uint8_t s_slave_addr[MB_SENSOR_COUNT] = { MB_SLAVE_ADDR_1, MB_SLAVE_ADDR_2 };
static mb_sensor_t   s_sensor[MB_SENSOR_COUNT];

typedef enum {
    MB_PHASE_IDLE = 0,   /* 空闲，等间隔时间到就发下一个请求 */
    MB_PHASE_WAIT        /* 请求已发出，等回包或超时 */
} mb_phase_t;

static mb_phase_t s_phase = MB_PHASE_IDLE;
static uint8_t    s_idx;        /* 正在轮询哪个从站 */
static uint8_t    s_retry;      /* 当前从站连续失败次数 */
static uint32_t   s_tick;       /* 进入当前状态的时间戳 */
static uint8_t    s_frame[64];  /* 回包暂存 */

/*----------------------------------------------- CRC16/MODBUS -------------------------------------------
 * 初值 0xFFFF，多项式 0xA001（0x8005 反射），帧里低字节在前。
 * 只覆盖「地址..数据」，不含 CRC 本身。
 */
static uint16_t mb_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFFU;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= buf[i];
        for (uint8_t bit = 0; bit < 8U; bit++)
        {
            if ((crc & 1U) != 0U)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/*----------------------------------------------- 发请求 -------------------------------------------*/

static void mb_send_request(uint8_t idx)
{
    uint8_t  req[8];
    uint16_t crc;

    req[0] = s_slave_addr[idx];
    req[1] = MB_REG_FUNC;
    req[2] = (uint8_t)(MB_REG_START >> 8);
    req[3] = (uint8_t)(MB_REG_START & 0xFFU);
    req[4] = (uint8_t)(MB_REG_COUNT >> 8);
    req[5] = (uint8_t)(MB_REG_COUNT & 0xFFU);

    crc    = mb_crc16(req, 6U);
    req[6] = (uint8_t)(crc & 0xFFU);
    req[7] = (uint8_t)(crc >> 8);

    RS485_FlushRx();                /* 先扔掉上一轮的残包 */
    (void)RS485_Send(req, sizeof req);
}

/*----------------------------------------------- 解析回包 -------------------------------------------
 * 正常回包：addr(1) func(1) bytecount(1) data(2*count) crc(2)
 * 异常回包：func 会带上 0x80，这里直接当失败处理（重试/离线）。
 */
static bool mb_parse(uint8_t idx, const uint8_t *f, uint16_t n)
{
    uint16_t need = (uint16_t)(3U + 2U * MB_REG_COUNT + 2U);
    uint16_t crc;
    int16_t  t;
    uint16_t h;

    if (n != need)                                        { return false; }
    if (f[0] != s_slave_addr[idx])                        { return false; }
    if (f[1] != MB_REG_FUNC)                              { return false; }
    if (f[2] != (uint8_t)(2U * MB_REG_COUNT))             { return false; }

    crc = (uint16_t)(f[n - 2U] | (uint16_t)(f[n - 1U] << 8));
    if (crc != mb_crc16(f, (uint16_t)(n - 2U)))            { return false; }

    t = (int16_t)(((uint16_t)f[3] << 8) | (uint16_t)f[4]);   /* 温度，有符号 x10 */
    h = (uint16_t)(((uint16_t)f[5] << 8) | (uint16_t)f[6]);  /* 湿度，无符号 x10 */

    s_sensor[idx].temp = (float)t / 10.0f;
    s_sensor[idx].humi = (float)h / 10.0f;
    return true;
}

/*----------------------------------------------- 对外 ----------------------------------------------*/

void Modbus_Init(void)
{
    for (uint8_t i = 0; i < MB_SENSOR_COUNT; i++)
    {
        s_sensor[i].temp   = 0.0f;
        s_sensor[i].humi   = 0.0f;
        s_sensor[i].online = false;
    }

    RS485_Init(MB_BAUDRATE);

    s_idx   = 0;
    s_retry = 0;
    s_phase = MB_PHASE_IDLE;
    s_tick  = HAL_GetTick();
}

void Modbus_Poll(void)
{
    uint32_t now = HAL_GetTick();

    switch (s_phase)
    {
    case MB_PHASE_IDLE:
        if ((now - s_tick) >= MB_POLL_GAP_MS)
        {
            mb_send_request(s_idx);
            s_tick  = now;
            s_phase = MB_PHASE_WAIT;
        }
        break;

    case MB_PHASE_WAIT:
    {
        uint16_t n  = RS485_GetFrame(s_frame, (uint16_t)sizeof s_frame);
        bool     ok = false;

        if (n > 0U)
        {
            ok = mb_parse(s_idx, s_frame, n);
        }
        else if ((now - s_tick) < MB_TIMEOUT_MS)
        {
            break;                  /* 回包还没来，也没到超时，下一轮再看 */
        }

        if (ok)
        {
            s_sensor[s_idx].online = true;
            s_retry = 0;
            s_idx   = (uint8_t)((s_idx + 1U) % MB_SENSOR_COUNT);
        }
        else
        {
            s_retry++;
            if (s_retry >= MB_MAX_RETRY)
            {
                s_sensor[s_idx].online = false;
                s_retry = 0;
                s_idx   = (uint8_t)((s_idx + 1U) % MB_SENSOR_COUNT);
            }
            /* 没到上限就保持 s_idx 不变，下一轮重发同一个从站 */
        }

        s_tick  = now;
        s_phase = MB_PHASE_IDLE;
        break;
    }

    default:
        s_phase = MB_PHASE_IDLE;
        break;
    }
}

const mb_sensor_t *Modbus_GetSensor(uint8_t idx)
{
    if (idx >= MB_SENSOR_COUNT)
    {
        return 0;
    }
    return &s_sensor[idx];
}
