/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "stm32f4_discovery.h"
/* Kernel includes. */
#include "stm32f4xx.h"
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"

/*-----------------------------------------------------------*/
#define mainQUEUE_LENGTH 100

#define amber  	0
#define green  	1
#define red  	2
#define blue  	3

#define amber_led	LED3
#define green_led	LED4
#define red_led		LED5
#define blue_led	LED6

#define RED_LIGHT GPIO_Pin_0
#define YELLOW_LIGHT GPIO_Pin_1
#define GREEN_LIGHT GPIO_Pin_2
#define SR_CLK GPIO_Pin_7
#define SR_DATA GPIO_Pin_6
#define SR_RST GPIO_Pin_8
#define ADC_IN	GPIO_Pin_3

#define BEFORE_STOP_LINE 0xff
#define STOP_LINE_POS 11
#define TOP_CAR_POS 19




enum LightState{
	LightRed = 0,
	LightGreen,
	LightYellow
};

static void prvSetupHardware( void );
static void trafficFlowAdjustmentTask (void *pvParameters );
static void systemDisplayTask (void * pvParameters );
static void trafficLightStateTask (void * pvParameters);
static void gpioSetup (void);
static void adcSetup(void);
static uint16_t adcConvert(void);
static void setShiftRegister(uint32_t bitmap, enum LightState lights);
static void ShiftRegisterPulse(uint32_t bit);
static void timerCallback (TimerHandle_t xTimer);
static void updateTrafficLight(enum LightState lights);


xQueueHandle xQueue_handle = 0;
xQueueHandle xNewCarQueue_handle = 0;
xQueueHandle xLightAdjustQueue_handle = 0;
xQueueHandle xTimerExpired_handle = 0;
xQueueHandle xLightStateQueue_handle = 0;

/*-----------------------------------------------------------*/

int main(void)
{
	/* Initialise LEDs */
	STM_EVAL_LEDInit(amber_led);
	STM_EVAL_LEDInit(green_led);
	STM_EVAL_LEDInit(red_led);
	STM_EVAL_LEDInit(blue_led);

	/* Configure the system ready to run the demo.  The clock configuration can be done here if it was not done before main() was called. */
	prvSetupHardware();
	GPIO_ResetBits(GPIOC,RED_LIGHT);
	GPIO_ResetBits(GPIOC,YELLOW_LIGHT);
	GPIO_ResetBits(GPIOC,GREEN_LIGHT);
	GPIO_SetBits(GPIOC, SR_RST);
	GPIO_SetBits(GPIOC, SR_CLK);



	/* Create the queue used by the queue send and queue receive tasks.
	http://www.freertos.org/a00116.html */
	xQueue_handle = xQueueCreate(mainQUEUE_LENGTH, sizeof( uint16_t ) );
	xNewCarQueue_handle = xQueueCreate(10, sizeof(uint16_t));
	xLightAdjustQueue_handle = xQueueCreate(mainQUEUE_LENGTH, sizeof(uint16_t));
	xTimerExpired_handle = xQueueCreate(mainQUEUE_LENGTH, sizeof(uint16_t));
	xLightStateQueue_handle = xQueueCreate(mainQUEUE_LENGTH, sizeof(uint16_t));

	/* Add to the registry, for the benefit of kernel aware debugging. */
	vQueueAddToRegistry( xQueue_handle, "MainQueue" );
	vQueueAddToRegistry( xNewCarQueue_handle, "NewCarQueue" );
	vQueueAddToRegistry( xLightAdjustQueue_handle, "LightAdjustQueue" );
	vQueueAddToRegistry( xTimerExpired_handle, "TimerExpiredQueue");
	vQueueAddToRegistry( xLightStateQueue_handle, "LightStateQueue");

	/* Create a Timer*/
	TimerHandle_t xTimer = xTimerCreate("TimerExpired", pdMS_TO_TICKS(1000), pdFALSE, (void*)1, timerCallback);

	xTaskCreate( trafficFlowAdjustmentTask, "TrafficFLowAdjustment", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
	xTaskCreate( systemDisplayTask, "SystemDisplay", configMINIMAL_STACK_SIZE, NULL, 3, NULL);
	xTaskCreate( trafficLightStateTask, "TrafficLightState",  configMINIMAL_STACK_SIZE, (void*)xTimer, 2, NULL);

	/* Start the tasks and timer running. */
	vTaskStartScheduler();

	return 0;
}

/*-----------------------------------------------------------*/

/*
 * trafficFlowAdjustmentTask
 * Reads ADC values, maps data to a traffic rate, and outputs
 * the result to NewCarQueue and LightAdjustQueue
 */
static void trafficFlowAdjustmentTask (void *pvParameters )
{
	uint16_t adc_val;
	while(1){
		adc_val = adcConvert();
		//For new car - normalise to a percentage
		//for traffic light - just give the number
		float new_car_prob = adc_val/248.0*83 + 17; //17-100
		uint32_t new_car = 0;
		if(rand()%100 < new_car_prob){
			new_car = 1;
		}
		//Send the new car status (0 or 1)
		if(!xQueueSend(xNewCarQueue_handle, &new_car, 500)){
			printf("NewCarQueue send Failed!");
		}
		//Send adc data to LightAdjustQueue
		if(!xQueueSend(xLightAdjustQueue_handle, &adc_val, 500)){
			printf("LightAdjustQueue send Failed!");
		}
		vTaskDelay(pdMS_TO_TICKS(500));
	}
}

/*
 * systemDisplayTask
 * updates the lights and shift register
 * Receives data from NewCarQueue and LightStateQueue
 */
static void systemDisplayTask (void * pvParameters )
{
	uint32_t current_car_bitmap = 0x0;
	uint32_t new_car = 0;
	enum LightState lights = LightGreen;
	while(1){
		if(xQueueReceive(xNewCarQueue_handle, &new_car, 100)){
			//New car
			if(xQueueReceive(xLightStateQueue_handle, &lights, 100)){
				//received new light state
				printf("New Light!\n");
			}
			if(lights != LightGreen){
				//Yellow/Red light shifting
				int bits = current_car_bitmap >> STOP_LINE_POS;
				int i;
				for(i = 0; i < 7; i++) {
					if (bits & (1 << (i+1))) {
						if (!(bits & (1 << i))) {
							bits = bits | (1 << i);
							bits = bits & (BEFORE_STOP_LINE - (1 << (i+1)));
						}
					}
				}

				current_car_bitmap = ((bits & 0xff)<<STOP_LINE_POS) | (new_car << (TOP_CAR_POS-1)) | ((current_car_bitmap & 0x7ff)>>1);
				current_car_bitmap &= 0x7FFFF;
			}else{
				//Green Light Shifting
				current_car_bitmap = ((new_car&1) << TOP_CAR_POS) | ((current_car_bitmap >> 1)&0x7FFFF);
			}

			printf("Car Bitmap: %x\n", current_car_bitmap);
			//Output light data
			setShiftRegister(current_car_bitmap, lights);
			//Update Traffic Lights
			updateTrafficLight(lights);
		}
		vTaskDelay(pdMS_TO_TICKS(500));
	}
}

/*
 * trafficLightStateTask
 * Adjusts the period for red/green lights, starts and waits for light timer
 * Receives data from LightAdjustQueue and TimerExpired
 * Transfers data to LightStateQueue
 */
static void trafficLightStateTask (void * pvParameters)
{
	TimerHandle_t xTimer = (TimerHandle_t) pvParameters;
	enum LightState lights = LightGreen;
	uint32_t update_light;
	uint16_t new_rate;
	uint16_t adc_val;
	uint32_t red_light_period = 1000;// ms
	uint32_t green_light_period = 1000;// ms
	uint32_t yellow_light_period = 2000;// ms

	//Start the process, set to green, send to queue, and start the timer
	xTimerChangePeriod(xTimer, pdMS_TO_TICKS(green_light_period),0);
	xTimerStart(xTimer, 0);
	xQueueSend(xLightStateQueue_handle, &lights, 10);


	while(1){//loop checking for new light timing, and if a timer expired
		if(xQueueReceive(xLightAdjustQueue_handle, &adc_val, 10)){
			//Got a new light rate, adjust the periods
			new_rate = (uint16_t)(adc_val*100)/256;
			//when rate 0, red = 10000, green = 5000
			//when rate 100, red = 5000, green = 10000
			red_light_period = 5000 + (100-new_rate)*50;
			green_light_period = 5000 + (new_rate)*50;
		}
		if(xQueueReceive(xTimerExpired_handle, &update_light, 200)){
			//Timer expired, change the light
			lights = (lights+1)%3;
			if(lights == LightGreen){
				printf("Light changed to Green!\n");
				xTimerChangePeriod(xTimer, pdMS_TO_TICKS(green_light_period),0);
				xTimerStart(xTimer, 0);
				xQueueSend(xLightStateQueue_handle, &lights, 10);
			}else if (lights == LightYellow){
				printf("Light changed to Yellow!\n");
				xTimerChangePeriod(xTimer, pdMS_TO_TICKS(yellow_light_period),0);
				xTimerStart(xTimer, 0);
				xQueueSend(xLightStateQueue_handle, &lights, 10);
			} else if(lights == LightRed) {//RegLight
				printf("Light changed to Red!\n");
				xTimerChangePeriod(xTimer, pdMS_TO_TICKS(red_light_period),0);
				xTimerStart(xTimer, 0);
				xQueueSend(xLightStateQueue_handle, &lights, 10);
			}
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

/*
 * timerCallback
 * Function called when timer finishes,
 * pushing data into TimerExpiredQueue
 */
static void timerCallback (TimerHandle_t xTimer) {
	xQueueSend(xTimerExpired_handle, xTimer, 10);
}

/*
 * setShiftRegister
 * given a bitmap, shifts out the current car state onto shift register
 */
static void setShiftRegister(uint32_t bitmap, enum LightState lights)
{
	for(int i = 0; i < TOP_CAR_POS; i++){
		ShiftRegisterPulse((bitmap>>i) & 1);
	}
}

/*
 * ShiftRegisterPulse
 * given a bit to set, pulses the shift register
 */
static void ShiftRegisterPulse(uint32_t bit)
{
	GPIO_ResetBits(GPIOC, SR_CLK);
	if(bit)GPIO_SetBits(GPIOC, SR_DATA);
	else GPIO_ResetBits(GPIOC, SR_DATA);
	GPIO_SetBits(GPIOC, SR_CLK);

}

/*
 * updateTrafficLight
 * given the current light state, turn on the appropriate light
 */
static void updateTrafficLight(enum LightState lights)
{
	if(lights == LightGreen){
		GPIO_SetBits(GPIOC, GREEN_LIGHT);
		GPIO_ResetBits(GPIOC, RED_LIGHT);
		GPIO_ResetBits(GPIOC, YELLOW_LIGHT);
	}else if (lights == LightRed){
		GPIO_ResetBits(GPIOC, GREEN_LIGHT);
		GPIO_SetBits(GPIOC, RED_LIGHT);
		GPIO_ResetBits(GPIOC, YELLOW_LIGHT);
	}else if(lights == LightYellow){
		GPIO_ResetBits(GPIOC, GREEN_LIGHT);
		GPIO_ResetBits(GPIOC, RED_LIGHT);
		GPIO_SetBits(GPIOC, YELLOW_LIGHT);
	}else{
		//Nothing
	}
}

/*-----------------------------------------------------------*/

void vApplicationMallocFailedHook( void )
{
	/* The malloc failed hook is enabled by setting
	configUSE_MALLOC_FAILED_HOOK to 1 in FreeRTOSConfig.h.

	Called if a call to pvPortMalloc() fails because there is insufficient
	free memory available in the FreeRTOS heap.  pvPortMalloc() is called
	internally by FreeRTOS API functions that create tasks, queues, software 
	timers, and semaphores.  The size of the FreeRTOS heap is set by the
	configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( xTaskHandle pxTask, signed char *pcTaskName )
{
	( void ) pcTaskName;
	( void ) pxTask;

	/* Run time stack overflow checking is performed if
	configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
	function is called if a stack overflow is detected.  pxCurrentTCB can be
	inspected in the debugger if the task name passed into this function is
	corrupt. */
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
volatile size_t xFreeStackSpace;

	/* The idle task hook is enabled by setting configUSE_IDLE_HOOK to 1 in
	FreeRTOSConfig.h.

	This function is called on each cycle of the idle task.  In this case it
	does nothing useful, other than report the amount of FreeRTOS heap that
	remains unallocated. */
	xFreeStackSpace = xPortGetFreeHeapSize();

	if( xFreeStackSpace > 100 )
	{
		/* By now, the kernel has allocated everything it is going to, so
		if there is a lot of heap remaining unallocated then
		the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
		reduced accordingly. */
	}
}
/*-----------------------------------------------------------*/

static void prvSetupHardware( void )
{
	/* Ensure all priority bits are assigned as preemption priority bits.
	http://www.freertos.org/RTOS-Cortex-M3-M4.html */
	NVIC_SetPriorityGrouping( 0 );


	/* TODO: Setup the clocks, etc. here, if they were not configured before
	main() was called. */
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
	//TODO: ALso need ADC clock
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

	gpioSetup();
	adcSetup();

}

/*
 *
 */
static void gpioSetup (void )
{
	// GPIO Setup:
	// Set to Output, Push/Pull, Pull Up, at some speed
	GPIO_InitTypeDef gpio_init;
	gpio_init.GPIO_Mode = GPIO_Mode_OUT;
	gpio_init.GPIO_OType = GPIO_OType_PP;
	gpio_init.GPIO_Pin = RED_LIGHT | YELLOW_LIGHT | GREEN_LIGHT | SR_CLK | SR_DATA | SR_RST;
	gpio_init.GPIO_PuPd = GPIO_PuPd_UP;
	gpio_init.GPIO_Speed = GPIO_Speed_25MHz;

	//Pins for traffic light and shift register
	GPIO_Init(GPIOC,&gpio_init);

	//ADC input
	gpio_init.GPIO_Mode = GPIO_Mode_AN;
	gpio_init.GPIO_Pin = ADC_IN;
	GPIO_Init(GPIOC,&gpio_init);

}

static void adcSetup(void)
{
	ADC_InitTypeDef adc_init;
	adc_init.ADC_ContinuousConvMode = DISABLE;
	adc_init.ADC_DataAlign = ADC_DataAlign_Right;
	adc_init.ADC_Resolution = ADC_Resolution_6b;

	ADC_Init(ADC1,&adc_init);
	ADC_Cmd(ADC1, ENABLE);
	ADC_RegularChannelConfig(ADC1, ADC_Channel_13, 1, ADC_SampleTime_3Cycles);

}

static uint16_t adcConvert(void)
{
	ADC_SoftwareStartConv(ADC1);
	while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC));
	uint16_t converted_data = ADC_GetConversionValue(ADC1);
	return converted_data;

}