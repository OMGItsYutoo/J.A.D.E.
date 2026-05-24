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
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#define POOL_SIZE 4096
#define DMA_BLOCK_SIZE 512

#define BIG_BUF_SIZE 81200  // Dimensione del tuo buffer gigante (es. 100KB, occhio alla RAM!)
#define DMA_BUF_SIZE 16318    // Buffer per il DMA
#define WAVE_MESSAGE "wave\n"
#define GOOD_OL_TIMES "train\n"

static uint8_t lut_r[32];
static uint8_t lut_g[64];

void init_lut(void) {
    for(int i = 0; i < 32; i++)
        lut_r[i] = (uint8_t)(powf((float)i / 31.0f, 0.5f) * 31.0f);
    for(int i = 0; i < 64; i++)
        lut_g[i] = (uint8_t)(powf((float)i / 63.0f, 0.5f) * 63.0f);
}

uint8_t dma_rx_buf[DMA_BUF_SIZE];
uint8_t big_rx_buf[BIG_BUF_SIZE];

uint16_t dma_buf[2][DMA_BLOCK_SIZE];
volatile uint8_t buf_idx=0;

uint8_t jdec_pool[POOL_SIZE];     // Memoria di lavoro per il decoder
volatile uint32_t big_wr_ptr = 0;
uint32_t big_rd_ptr = 0;     // Sostituisce il vecchio rd_ptr

JDEC jdec;
volatile uint8_t spi_dma_ready = 1;

uint16_t joystickdata[2];
uint8_t flash_mode = 0;

char uart_msg[50];
int buf_len;


void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
	if (GPIO_Pin == GPIO_PIN_0) {
		static uint8_t press_count = 0;
	    static uint32_t last_press_tick = 0;

	    uint32_t now = HAL_GetTick();

	    // Debounce: ignora pressioni più veloci di 120ms
	    //se quando premo ne conta di più vuol dire che non sta filtrando abbastanza quindi alzare tempo
	    if (now - last_press_tick < 120) {
	        return;
	    }

	    // Se è passato più di 1 secondo dall'ultima pressione, resetta
	    if (now - last_press_tick > 700) {
	        press_count = 0;
	    }

	    last_press_tick = now;
	    press_count++;

	    if (press_count >= 8) {
	        HAL_UART_Transmit(&huart4, (uint8_t*)GOOD_OL_TIMES, sizeof(GOOD_OL_TIMES), 10);
	        press_count = 0;
	    } else {
	        HAL_UART_Transmit(&huart4, (uint8_t*)WAVE_MESSAGE, sizeof(WAVE_MESSAGE), 10);
	    }
	}
}


// Funzione helper per copiare nel buffer circolare gigante
void Copy_To_Big_Buffer(uint8_t* src, uint16_t len) {
    for(uint16_t i = 0; i < len; i++) {
        big_rx_buf[big_wr_ptr] = src[i];
        big_wr_ptr = (big_wr_ptr + 1) % BIG_BUF_SIZE;
    }
}

// Chiamata quando la prima metà del buffer DMA è piena
void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart) {
    if(huart->Instance == UART4) {
        Copy_To_Big_Buffer(&dma_rx_buf[0], DMA_BUF_SIZE / 2);
    }
}

// Chiamata quando la seconda metà del buffer DMA è piena
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    if(huart->Instance == UART4) {
        Copy_To_Big_Buffer(&dma_rx_buf[DMA_BUF_SIZE / 2], DMA_BUF_SIZE / 2);
    }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
    if (hspi == &hspi1) {
        LCD_CS_1;
        spi_dma_ready = 1;
    }
}

/* --- Funzione di Input per TJpgDec --- */
size_t in_func(JDEC* jd, uint8_t* buff, size_t nbyte) {
    size_t read_bytes = 0;
    while (read_bytes < nbyte) {
        // Leggi il puntatore di scrittura aggiornato dagli interrupt
        uint32_t wr_ptr = big_wr_ptr;

        uint32_t available = (wr_ptr - big_rd_ptr + BIG_BUF_SIZE) % BIG_BUF_SIZE;
        if (available == 0) continue; // Aspetta nuovi dati

        uint32_t to_read = nbyte - read_bytes;
        uint32_t until_wrap = BIG_BUF_SIZE - big_rd_ptr;
        uint32_t chunk = to_read < available ? to_read : available;
        if (chunk > until_wrap) chunk = until_wrap;

        if (buff) memcpy(&buff[read_bytes], &big_rx_buf[big_rd_ptr], chunk);
        big_rd_ptr = (big_rd_ptr + chunk) % BIG_BUF_SIZE;
        read_bytes += chunk;
    }
    return read_bytes;
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
  init_lut();
  MX_UART4_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  HAL_ADC_Start_DMA(&hadc1, (uint32_t*)joystickdata, 2);
  HAL_UART_Receive_DMA(&huart4, dma_rx_buf, DMA_BUF_SIZE);


  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
  HAL_Delay(100);
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(200);

  LCD_Init(U2D_L2R, 255);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  /*while (1) {
	  LCD_SetWindow(0, 0, 479, 319);
	  LCD_FillDMA(RED, 480 * 320);

	  HAL_Delay(1000);

	  LCD_SetWindow(0, 0, 479, 319);
	  	  LCD_FillDMA(BLUE, 480 * 320);

	  	  HAL_Delay(1000);


  }*/

  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	  uint8_t byte;
	  uint8_t last_byte = 0;
	  found = 0;

	  // --- 1. Sincronizzazione al marker JPEG (0xFFD8) ---
	  while (!found) {
		  in_func(NULL, &byte, 1);
		  if (last_byte == 0xFF && byte == 0xD8) {
			  found = 1;
		  }
		  last_byte = byte;
		  len = sprintf(uart_msg, "%u,%u\n", joystickdata[0], joystickdata[1]);
		  HAL_UART_Transmit(&huart4, (uint8_t*)uart_msg, len, 10);
	  }



	  // --- 2. Riavvolgimento puntatore ---
	  // Riportiamo rd_ptr indietro di 2 byte per far vedere 0xFFD8 a jd_prepare
	  if (big_rd_ptr >= 2) {
		big_rd_ptr -= 2;
	} else {
		big_rd_ptr = BIG_BUF_SIZE - (2 - big_rd_ptr);
	}
	  // --- 3. Decompressione ---
	  res = jd_prepare(&jdec, in_func, jdec_pool, POOL_SIZE, NULL);
	  if (res == JDR_OK) {
		  // Parametri: oggetto, output_func, scale (0=1/1, 1=1/2, 2=1/4)
		  jd_decomp(&jdec, out_func, 0);
	  }

	  // Finito il frame, pulisce i flag e ricomincia a cercare il prossimo header
	  while (!spi_dma_ready);
	  LCD_CS_1;
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
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
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
