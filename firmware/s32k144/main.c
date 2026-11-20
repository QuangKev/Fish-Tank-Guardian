#include "sdk_project_config.h"
#include "lcd_12864_library.h"
#include "peripherals_lpuart_1.h"
#include <stdio.h>

/*
 * ================== HARDWARE MAP ==================
 *
 * UART ESP32 <-> S32K:
 * ESP32 TX2 GPIO17 -> S32K PTD6 / LPUART2_RX
 * ESP32 RX2 GPIO16 <- S32K PTD7 / LPUART2_TX
 * Common GND is required.
 *
 * Stepper motor with ULN2003:
 * IN1 = PTD14
 * IN2 = PTD13
 * IN3 = PTD12
 * IN4 = PTD11
 *
 * Buzzer:
 * PTD5
 *
 * Physical button:
 * PTD8
 * One button pin -> PTD8
 * Other button pin -> GND
 * Internal pull-up is used.
 *
 * LCD 12864:
 * Uses previous SPI configuration.
 *
 * TDS:
 * Uses previous ADC configuration.
 */

/* ================== PIN CONFIG ================== */

#define MOTOR_IN1_PIN      14U
#define MOTOR_IN2_PIN      13U
#define MOTOR_IN3_PIN      12U
#define MOTOR_IN4_PIN      11U

#define BUZZER_PIN         5U

#define BUTTON_PORT        PTD
#define BUTTON_PORT_BASE   PORTD
#define BUTTON_PIN         8U

/* ================== TASK CONFIG ================== */

#define TDS_SAMPLE_COUNTER_LIMIT        2000U

/* ================== GLOBAL VARIABLES ================== */

uint16_t adcRawValue = 0U;
float tdsVoltage = 0.0f;
int tdsValue = 0;
char numStr[20];

uint8_t rxData = 0U;

volatile uint8_t feedingBusy = 0U;
volatile uint8_t buttonLocked = 0U;

uint32_t tdsSampleCounter = 0U;

/* ================== MOTOR STEP SEQUENCE ================== */

const uint8_t step_sequence[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1}
};

/* ================== PROTOTYPES ================== */

void Delay_ms(uint32_t ms);

void UART_SendChar(uint8_t data);
void UART_SendString(const char *str);
void UART_ProcessReceive(void);
void ProcessCommand(uint8_t cmd);
void UART_FlushInput(void);

void Motor_Init(void);
void Motor_Release(void);
void Motor_SetStep(uint8_t stepIndex);
void Motor_RotateAngle(float angle, int direction);
void FeedFish(void);

void Buzzer_Init(void);
void Buzzer_On(void);
void Buzzer_Off(void);
void Buzzer_Toggle(void);
void Buzzer_Beep(uint32_t ms);

void Button_Init(void);
uint8_t Button_IsPressed(void);
void Button_Task(void);

void LCD_ClearArea(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2);
void LCD_ClearLine40(void);
void LCD_ShowStaticFrame(void);
void LCD_ShowMessage(const char *msg);
void LCD_UpdateTDS(void);

void TDS_Read(void);
const char* TDS_GetStatus(int value);
void TDS_SendToESP32(void);
void TDS_Task(void);

/* ================== DELAY ================== */

void Delay_ms(uint32_t ms)
{
    volatile uint32_t i;
    volatile uint32_t j;

    for (i = 0U; i < ms; i++)
    {
        for (j = 0U; j < 4000U; j++)
        {
            __asm("nop");
        }
    }
}

/* ================== UART ================== */

void UART_SendChar(uint8_t data)
{
    LPUART_DRV_SendDataBlocking(INST_LPUART_1,
                                &data,
                                1U,
                                1000U);
}

void UART_SendString(const char *str)
{
    while (*str != '\0')
    {
        UART_SendChar((uint8_t)(*str));
        str++;
    }
}

void UART_FlushInput(void)
{
    uint8_t dummy;

    while (LPUART_DRV_ReceiveDataBlocking(INST_LPUART_1,
                                          &dummy,
                                          1U,
                                          1U) == STATUS_SUCCESS)
    {
        /* Drop pending byte. */
    }
}

/*
 * Commands from ESP32:
 * P = ping
 * F = feed
 * T = send latest cached TDS value
 */
void UART_ProcessReceive(void)
{
    if (LPUART_DRV_ReceiveDataBlocking(INST_LPUART_1,
                                       &rxData,
                                       1U,
                                       2U) == STATUS_SUCCESS)
    {
        if ((rxData == '\n') || (rxData == '\r'))
        {
            return;
        }

        ProcessCommand(rxData);
    }
}

void ProcessCommand(uint8_t cmd)
{
    if (cmd == 'P')
    {
        UART_SendString("PONG\n");
    }
    else if (cmd == 'F')
    {
        if (feedingBusy == 1U)
        {
            UART_SendString("BUSY:FEEDING\n");
            return;
        }

        /*
         * Send ACK multiple times before starting the motor.
         * This helps ESP32 receive the acknowledgement more reliably.
         */
        UART_SendString("FEED_ACK\n");
        Delay_ms(30U);
        UART_SendString("FEED_ACK\n");
        Delay_ms(30U);
        UART_SendString("FEED_ACK\n");

        LCD_ShowMessage("FEEDING");

        FeedFish();

        LCD_ShowMessage("FEED DONE");

        /*
         * Send DONE multiple times as well.
         */
        UART_SendString("FEED_DONE\n");
        Delay_ms(30U);
        UART_SendString("FEED_DONE\n");

        UART_FlushInput();
    }
    else if (cmd == 'T')
    {
        if (feedingBusy == 1U)
        {
            UART_SendString("BUSY:FEEDING\n");
            return;
        }

        /*
         * Send cached TDS value quickly.
         * Do not read ADC or update LCD inside this command handler.
         */
        UART_SendString("TDS_ACK\n");

        TDS_SendToESP32();
        Delay_ms(20U);
        TDS_SendToESP32();
    }
    else
    {
        UART_SendString("UNKNOWN\n");
    }
}

/* ================== BUZZER ================== */

void Buzzer_Init(void)
{
    PINS_DRV_SetMuxModeSel(PORTD, BUZZER_PIN, PORT_MUX_AS_GPIO);
    PINS_DRV_SetPinDirection(PTD, BUZZER_PIN, GPIO_OUTPUT_DIRECTION);
    Buzzer_Off();
}

void Buzzer_On(void)
{
    PINS_DRV_WritePin(PTD, BUZZER_PIN, 1U);
}

void Buzzer_Off(void)
{
    PINS_DRV_WritePin(PTD, BUZZER_PIN, 0U);
}

void Buzzer_Toggle(void)
{
    PINS_DRV_TogglePins(PTD, 1UL << BUZZER_PIN);
}

void Buzzer_Beep(uint32_t ms)
{
    Buzzer_On();
    Delay_ms(ms);
    Buzzer_Off();
}

/* ================== BUTTON ================== */

void Button_Init(void)
{
    PINS_DRV_SetMuxModeSel(BUTTON_PORT_BASE, BUTTON_PIN, PORT_MUX_AS_GPIO);
    PINS_DRV_SetPinDirection(BUTTON_PORT, BUTTON_PIN, GPIO_INPUT_DIRECTION);
}

uint8_t Button_IsPressed(void)
{
    if ((PINS_DRV_ReadPins(BUTTON_PORT) & (1UL << BUTTON_PIN)) == 0U)
    {
        return 1U;
    }

    return 0U;
}

void Button_Task(void)
{
    if (Button_IsPressed() == 1U)
    {
        Delay_ms(30U);

        if (Button_IsPressed() == 1U)
        {
            if (buttonLocked == 0U)
            {
                buttonLocked = 1U;

                if (feedingBusy == 0U)
                {
                    UART_SendString("BUTTON_FEED_ACK\n");
                    LCD_ShowMessage("BUTTON FEED");

                    FeedFish();

                    LCD_ShowMessage("FEED DONE");
                    UART_SendString("BUTTON_FEED_DONE\n");
                }
                else
                {
                    UART_SendString("BUSY:BUTTON_IGNORED\n");
                }
            }
        }
    }
    else
    {
        buttonLocked = 0U;
    }
}

/* ================== MOTOR ================== */

void Motor_Init(void)
{
    PINS_DRV_SetMuxModeSel(PORTD, MOTOR_IN1_PIN, PORT_MUX_AS_GPIO);
    PINS_DRV_SetMuxModeSel(PORTD, MOTOR_IN2_PIN, PORT_MUX_AS_GPIO);
    PINS_DRV_SetMuxModeSel(PORTD, MOTOR_IN3_PIN, PORT_MUX_AS_GPIO);
    PINS_DRV_SetMuxModeSel(PORTD, MOTOR_IN4_PIN, PORT_MUX_AS_GPIO);

    PINS_DRV_SetPinDirection(PTD, MOTOR_IN1_PIN, GPIO_OUTPUT_DIRECTION);
    PINS_DRV_SetPinDirection(PTD, MOTOR_IN2_PIN, GPIO_OUTPUT_DIRECTION);
    PINS_DRV_SetPinDirection(PTD, MOTOR_IN3_PIN, GPIO_OUTPUT_DIRECTION);
    PINS_DRV_SetPinDirection(PTD, MOTOR_IN4_PIN, GPIO_OUTPUT_DIRECTION);

    Motor_Release();
}

void Motor_Release(void)
{
    PINS_DRV_WritePin(PTD, MOTOR_IN1_PIN, 0U);
    PINS_DRV_WritePin(PTD, MOTOR_IN2_PIN, 0U);
    PINS_DRV_WritePin(PTD, MOTOR_IN3_PIN, 0U);
    PINS_DRV_WritePin(PTD, MOTOR_IN4_PIN, 0U);
}

void Motor_SetStep(uint8_t stepIndex)
{
    PINS_DRV_WritePin(PTD, MOTOR_IN1_PIN, step_sequence[stepIndex][0]);
    PINS_DRV_WritePin(PTD, MOTOR_IN2_PIN, step_sequence[stepIndex][1]);
    PINS_DRV_WritePin(PTD, MOTOR_IN3_PIN, step_sequence[stepIndex][2]);
    PINS_DRV_WritePin(PTD, MOTOR_IN4_PIN, step_sequence[stepIndex][3]);
}

/*
 * direction:
 * 1 = clockwise
 * 0 = counter-clockwise
 */
void Motor_RotateAngle(float angle, int direction)
{
    uint32_t total_steps = (uint32_t)((angle * 4096.0f) / 360.0f);
    int step_index = 0;

    Buzzer_On();

    for (uint32_t i = 0U; i < total_steps; i++)
    {
        Motor_SetStep((uint8_t)step_index);

        /*
         * Motor step delay.
         * If the motor vibrates but does not rotate, increase 3U to 5U.
         */
        Delay_ms(3U);

        Buzzer_Toggle();

        if (direction == 1)
        {
            step_index++;

            if (step_index > 7)
            {
                step_index = 0;
            }
        }
        else
        {
            step_index--;

            if (step_index < 0)
            {
                step_index = 7;
            }
        }
    }

    Motor_Release();
    Buzzer_Off();
}

void FeedFish(void)
{
    if (feedingBusy == 1U)
    {
        return;
    }

    feedingBusy = 1U;

    /*
     * Feed sequence:
     * 1. Rotate 180 degrees forward
     * 2. Wait 0.5 second
     * 3. Rotate 180 degrees backward
     */
    Motor_RotateAngle(180.0f, 1);
    Delay_ms(500U);
    Motor_RotateAngle(180.0f, 0);

    feedingBusy = 0U;
}

/* ================== LCD ================== */

void LCD_ClearArea(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2)
{
    for (uint8_t x = x1; x < x2; x++)
    {
        for (uint8_t y = y1; y < y2; y++)
        {
            LCD_DrawPixel(x, y, false);
        }
    }
}

void LCD_ClearLine40(void)
{
    LCD_ClearArea(0U, 40U, 128U, 48U);
}

void LCD_ShowStaticFrame(void)
{
    LCD_GraphicClear();

    LCD_GraphicPrint(0U, 16U, "FISH TANK MONITOR");
    LCD_GraphicPrint(16U, 24U, "TDS:         ppm");
    LCD_GraphicPrint(16U, 32U, "STATUS:");
    LCD_GraphicPrint(16U, 40U, "MSG:");

    LCD_GraphicUpdate();
}

void LCD_ShowMessage(const char *msg)
{
    LCD_ClearLine40();
    LCD_GraphicPrint(46U, 40U, (char *)msg);
    LCD_GraphicUpdate();
}

void LCD_UpdateTDS(void)
{
    LCD_ClearArea(46U, 24U, 82U, 32U);
    LCD_ClearArea(72U, 32U, 128U, 40U);

    sprintf(numStr, "%4d", tdsValue);
    LCD_GraphicPrint(46U, 24U, numStr);
    LCD_GraphicPrint(72U, 32U, (char *)TDS_GetStatus(tdsValue));

    LCD_GraphicUpdate();
}

/* ================== TDS ================== */

void TDS_Read(void)
{
    ADC_DRV_ConfigChan(0U, 0U, &adc_config_1_ChnConfig0);
    ADC_DRV_WaitConvDone(0U);
    ADC_DRV_GetChanResult(0U, 0U, &adcRawValue);

    tdsVoltage = ((float)adcRawValue / 4095.0f) * 5.0f;

    tdsValue = (int)((133.42f * tdsVoltage * tdsVoltage * tdsVoltage
                    - 255.86f * tdsVoltage * tdsVoltage
                    + 857.39f * tdsVoltage) * 0.5f);

    if (tdsValue < 0)
    {
        tdsValue = 0;
    }
}

const char* TDS_GetStatus(int value)
{
    if (value < 300)
    {
        return "GOOD";
    }
    else if (value <= 600)
    {
        return "NORMAL";
    }
    else
    {
        return "DIRTY";
    }
}

void TDS_SendToESP32(void)
{
    char buffer[40];

    sprintf(buffer, "TDS:%d:%s\n", tdsValue, TDS_GetStatus(tdsValue));
    UART_SendString(buffer);
}

void TDS_Task(void)
{
    if (feedingBusy == 1U)
    {
        return;
    }

    tdsSampleCounter++;

    if (tdsSampleCounter >= TDS_SAMPLE_COUNTER_LIMIT)
    {
        tdsSampleCounter = 0U;

        TDS_Read();
        LCD_UpdateTDS();
    }
}

/* ================== MAIN ================== */

int main(void)
{
    (void)CLOCK_DRV_Init(&clockMan1_InitConfig0);
    PINS_DRV_Init(NUM_OF_CONFIGURED_PINS0, g_pin_mux_InitConfigArr0);

    /*
     * Initialize LCD SPI.
     */
    (void)SPI_MasterInit(&spi_pal_1_Instance, &LPSPI_0_MasterInitConfig0);

    /*
     * Initialize ADC.
     */
    ADC_DRV_ConfigConverter(0U, &adc_config_1_ConvConfig0);

    /*
     * Initialize UART.
     */
    LPUART_DRV_Init(INST_LPUART_1,
                    &lpUartState2,
                    &lpuart_2_InitConfig0);

    Motor_Init();
    Buzzer_Init();
    Button_Init();

    LCD_Init();
    LCD_GraphicMode();
    LCD_ShowStaticFrame();

    Buzzer_Beep(120U);
    Delay_ms(120U);
    Buzzer_Beep(120U);

    TDS_Read();
    LCD_UpdateTDS();

    LCD_ShowMessage("READY");

    /*
     * Send READY multiple times.
     * ESP32 will report it only once.
     */
    Delay_ms(500U);
    UART_SendString("READY\n");
    Delay_ms(200U);
    UART_SendString("READY\n");
    Delay_ms(200U);
    UART_SendString("READY\n");

    while (1)
    {
        UART_ProcessReceive();

        Button_Task();

        TDS_Task();

        Delay_ms(1U);
    }

    return 0;
}
