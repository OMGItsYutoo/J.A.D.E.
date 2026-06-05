/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "tjpgd.h"
#include <math.h>
#include "ILI9486.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

SPI_HandleTypeDef hspi1;
DMA_HandleTypeDef hdma_spi1_tx;

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart4;
DMA_HandleTypeDef hdma_uart4_rx;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_UART4_Init(void);
static void MX_SPI1_Init(void);
static void MX_TIM3_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#define POOL_SIZE 4096
#define DMA_BLOCK_SIZE 1024

#define WAVE_MESSAGE "wave\n"
#define GOOD_OL_TIMES "train\n"

#define NUM_BUFFERS 4

static uint8_t lut_r[32];
static uint8_t lut_g[64];

void init_lut(void) {
    for(int i = 0; i < 32; i++)
        lut_r[i] = (uint8_t)(powf((float)i / 31.0f, 0.6f) * 31.0f);
    for(int i = 0; i < 64; i++)
        lut_g[i] = (uint8_t)(powf((float)i / 63.0f, 0.7f) * 63.0f);
}

#define MAX_JPEG_SIZE 20000 // 60 KB max per frame
uint8_t jpeg_buf[NUM_BUFFERS][MAX_JPEG_SIZE];

#define BUF_FREE   0
#define BUF_DMA    1
#define BUF_READY  2

// Tutti i buffer nascono liberi, tranne il primo che diamo subito al DMA
volatile uint8_t buffer_state[NUM_BUFFERS] = {BUF_DMA, BUF_FREE,BUF_FREE,BUF_FREE};
volatile uint32_t frame_lengths[NUM_BUFFERS] = {0};

volatile uint8_t dma_write_idx = 0;      // Dove scrive il DMA
uint8_t locked_cpu_idx = 0;              // Buffer attualmente posseduto dalla CPU
uint32_t locked_frame_len = 0;           // Lunghezza protetta per la CPU
uint32_t jpeg_read_offset = 0;           // Usato da in_func per scorrere l'array

uint16_t dma_buf[2][DMA_BLOCK_SIZE];
volatile uint8_t buf_idx=0;

__attribute__((aligned(4))) uint8_t jdec_pool[POOL_SIZE]; // Memoria allineata!
JDEC jdec;
volatile uint8_t spi_dma_ready = 1;

uint16_t joystickdata[2];
char timer_uart_msg[25];
volatile uint8_t joystick_tx_busy = 0;

uint8_t cpu_jpeg_buf[MAX_JPEG_SIZE];
uint32_t cpu_frame_len;

volatile uint32_t btn_last_press_tick = 0;
volatile uint8_t btn_press_count = 0;

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance == TIM3) {
        if (joystick_tx_busy == 0) {
            joystick_tx_busy = 1;
            int len = sprintf(timer_uart_msg, "%u,%u\n", joystickdata[0], joystickdata[1]);
            HAL_UART_Transmit_IT(&huart4, (uint8_t*)timer_uart_msg, len);
        }
    }

    // --- NUOVA LOGICA TIM2 PER FINESTRA CLICK ---
    if (htim->Instance == TIM2) {
        // Il tempo è scaduto! Spegniamo il timer in modalità One-Shot
        HAL_TIM_Base_Stop_IT(&htim2);

        if (btn_press_count == 1) {
            // Click singolo confermato!
            HAL_UART_Transmit_IT(&huart4, (uint8_t*)WAVE_MESSAGE, sizeof(WAVE_MESSAGE));
        }
        else if (btn_press_count > 1) {
            // Ha premuto 2 o 3 volte ma si è fermato prima di 4.
            // Decidi tu se mandare comunque "wave" o fare altro.
            HAL_UART_Transmit_IT(&huart4, (uint8_t*)WAVE_MESSAGE, sizeof(WAVE_MESSAGE));
        }

        // Reset del contatore per la prossima sequenza
        btn_press_count = 0;
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == UART4) {
        joystick_tx_busy = 0;
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    if (huart->Instance == UART4) {

        uint8_t next_idx = (dma_write_idx + 1) % NUM_BUFFERS;

        if (buffer_state[next_idx] == BUF_FREE) {
            /* Commit del frame corrente, avanza al prossimo slot */
            frame_lengths[dma_write_idx] = Size;
            buffer_state[dma_write_idx] = BUF_READY;

            dma_write_idx = next_idx;
            buffer_state[dma_write_idx] = BUF_DMA;
        }
        /* else: overflow → il DMA ricomincia sullo stesso buffer,
           scartando il frame parziale appena ricevuto. Perdita accettabile
           per uno stream live. */

        HAL_UARTEx_ReceiveToIdle_DMA(&huart4, jpeg_buf[dma_write_idx], MAX_JPEG_SIZE);
        __HAL_DMA_DISABLE_IT(&hdma_uart4_rx, DMA_IT_HT);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == UART4) {
        // Pulisci i flag di errore hardware
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);

        // Riavvia il DMA
        HAL_UARTEx_ReceiveToIdle_DMA(&huart4, jpeg_buf[dma_write_idx], MAX_JPEG_SIZE);
        __HAL_DMA_DISABLE_IT(&hdma_uart4_rx, DMA_IT_HT); // Disabilita sempre l'HT!
    }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == GPIO_PIN_0) {
        uint32_t now = HAL_GetTick();

        // 1. Debounce software: ignora rimbalzi sotto i 150ms
        if (now - btn_last_press_tick < 150) {
            return;
        }
        btn_last_press_tick = now;

        // 2. Incrementa il numero di click
        btn_press_count++;

        // 3. Controllo immediato: se siamo a 4 click, triggeriamo subito l'azione
        if (btn_press_count >= 4) {
            // CORREZIONE: Usa la funzione HAL per fermare l'interruzione e resettare lo stato
            HAL_TIM_Base_Stop_IT(&htim2);
            __HAL_TIM_SET_COUNTER(&htim2, 0); // Riporta il conteggio hardware a 0

            HAL_UART_Transmit_IT(&huart4, (uint8_t*)GOOD_OL_TIMES, sizeof(GOOD_OL_TIMES));
            btn_press_count = 0;
        }
        else {
            // 4. Se sono meno di 4 click, resetta il counter e fai ripartire in sicurezza
            __HAL_TIM_SET_COUNTER(&htim2, 0);
            HAL_TIM_Base_Start_IT(&htim2);
        }
    }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
	while (__HAL_SPI_GET_FLAG(hspi, SPI_FLAG_BSY) != RESET) {
			__NOP();
	}
	LCD_CS_1; // Ora è sicuro alzare il Chip Select
	spi_dma_ready = 1;
}


size_t in_func(JDEC* jd, uint8_t* buff, size_t nbyte) {
    // Usiamo cpu_frame_len invece del vecchio locked_frame_len!
    if (jpeg_read_offset >= cpu_frame_len) return 0;

    size_t to_read = nbyte;
    if (jpeg_read_offset + to_read > cpu_frame_len) {
        to_read = cpu_frame_len - jpeg_read_offset;
    }

    if (buff) {
        memcpy(buff, &cpu_jpeg_buf[jpeg_read_offset], to_read);
    }

    jpeg_read_offset += to_read;
    return to_read;
}

/* --- Funzione di Output per TJpgDec --- */
int out_func(JDEC* jd, void* bitmap, JRECT* rect) {
	while (spi_dma_ready == 0) __NOP();

	// 1. Imposta la finestra sul display
	LCD_SetWindow(rect->left, rect->top, rect->right, rect->bottom);

    // 3. Calcola numero di pixel (SPI a 16 bit invia Half-Words)
    uint32_t num_pixels = (rect->right - rect->left + 1) * (rect->bottom - rect->top + 1);

    uint16_t *pixels = (uint16_t*)bitmap;
    for(uint32_t i = 0; i < num_pixels; i++) {
        uint16_t p = pixels[i];
        uint8_t r = (p >> 11) & 0x1F;
        uint8_t g = (p >> 5)  & 0x3F;
        uint8_t b = p & 0x1F;

        pixels[i] = (lut_r[r] << 11) | (lut_g[g] << 5) | b;
    }
	// Salva indice buffer PRIMA di switchare
	uint8_t current_buf = buf_idx;
	memcpy(dma_buf[current_buf], bitmap, num_pixels*2);

	// Switch buffer per prossimo MCU
	buf_idx = 1 - buf_idx;

	// Trasmetti dati pixel
	spi_dma_ready = 0;
	LCD_DC_1;      // Dati
	LCD_CS_0;
	HAL_SPI_Transmit_DMA(&hspi1, (uint8_t*)dma_buf[current_buf], num_pixels);

    return 1; // Continua decodifica
}

int len;
uint8_t found=0;
JRESULT res;
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_UART4_Init();
  MX_SPI1_Init();
  MX_TIM3_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  HAL_ADC_Start_DMA(&hadc1, (uint32_t*)joystickdata, 2);

  HAL_TIM_Base_Start_IT(&htim3);

  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
  HAL_Delay(100);
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(200);

  LCD_Init(U2D_L2R, 255);
  init_lut();

  HAL_UARTEx_ReceiveToIdle_DMA(&huart4, jpeg_buf[dma_write_idx], MAX_JPEG_SIZE);
    __HAL_DMA_DISABLE_IT(&hdma_uart4_rx, DMA_IT_HT);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  uint8_t main_read_idx = 0; // Il main parte dallo slot 0
  while (1)
    {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  if (buffer_state[main_read_idx] == BUF_READY) {

			  // IL BUFFER È DELLA CPU! Nessun rischio di sovrascrittura.
		  	  cpu_frame_len = frame_lengths[main_read_idx];

		      memcpy(cpu_jpeg_buf,
		             jpeg_buf[main_read_idx],
		             cpu_frame_len);

		      // Libero IMMEDIATAMENTE
		      buffer_state[main_read_idx] = BUF_FREE;

			main_read_idx = (main_read_idx + 1) % NUM_BUFFERS;

			jpeg_read_offset = 0;

			// Ricerca header
			while (jpeg_read_offset < cpu_frame_len - 1) {
				if (cpu_jpeg_buf[jpeg_read_offset] == 0xFF &&
						cpu_jpeg_buf[jpeg_read_offset + 1] == 0xD8) {
					break;
				}
				jpeg_read_offset++;
			}

			if (jpeg_read_offset < cpu_frame_len - 1) {
				res = jd_prepare(&jdec, in_func, jdec_pool, POOL_SIZE, NULL);
				if (res == JDR_OK) {
					jd_decomp(&jdec, out_func, 0);
				}
			}

			// HO FINITO CON IL JPEG! Libero lo slot per il DMA della UART
			// Passo ad aspettare il buffer successivo

		}
    }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV8;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ENABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 2;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SEQ_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = 2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_16BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 8399;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4999;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 8399;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 499;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * @brief UART4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_UART4_Init(void)
{

  /* USER CODE BEGIN UART4_Init 0 */

  /* USER CODE END UART4_Init 0 */

  /* USER CODE BEGIN UART4_Init 1 */

  /* USER CODE END UART4_Init 1 */
  huart4.Instance = UART4;
  huart4.Init.BaudRate = 2000000;
  huart4.Init.WordLength = UART_WORDLENGTH_9B;
  huart4.Init.StopBits = UART_STOPBITS_1;
  huart4.Init.Parity = UART_PARITY_EVEN;
  huart4.Init.Mode = UART_MODE_TX_RX;
  huart4.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart4.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN UART4_Init 2 */

  /* USER CODE END UART4_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream2_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream2_IRQn);
  /* DMA2_Stream4_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream4_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream4_IRQn);
  /* DMA2_Stream5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream5_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream5_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, LCD_DC_Pin|LCD_CS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : LCD_DC_Pin LCD_CS_Pin */
  GPIO_InitStruct.Pin = LCD_DC_Pin|LCD_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : LCD_RST_Pin */
  GPIO_InitStruct.Pin = LCD_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LCD_RST_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PB0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 5);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
