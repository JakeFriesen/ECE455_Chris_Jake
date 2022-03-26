/*ECE 455 Project 2 - Deadline Driven Scheduler
    Chris Dunn
    Jake Friesen
*/
/* Standard includes */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stm32f4_discovery.h"
/* Kernel includes */
#include "stm32f4xx.h"
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"
/*-----------------------------------------------------------*/
#define DD_SCHEDULER_PRIO configMAX_PRIORITIES - 1
#define GENERATOR_PRIO configMAX_PRIORITIES - 2
#define HIGH_PRIO configMAX_PRIORITIES - 3
#define LOW_PRIO configMAX_PRIORITIES - 4
#define Aux_STACK_SIZE ((unsigned short)30)
#define Mul_Divider 00111
#define schedule_QUEUE_LENGTH 3
#define generator_QUEUE_LENGTH 3
#define message_QUEUE_LENGTH 3

#define RED_LIGHT GPIO_Pin_0
#define YELLOW_LIGHT GPIO_Pin_1
#define GREEN_LIGHT GPIO_Pin_2
#define SR_CLK GPIO_Pin_7
#define SR_DATA GPIO_Pin_6
#define SR_RST GPIO_Pin_8
#define ADC_IN	GPIO_Pin_3

// Then you need to define structures here :
enum Task_Types {
    periodic,
    aperiodic
} Task_Types;

//Request Types
enum Request_Types
{
    create,
    delete,
    active_task_list,
    completed_task_list,
    overdue_task_list
} Request_Types;

typedef struct
{
    TaskHandle_t t_handle;
    uint32_t task_id;
    enum Task_Types type;
    uint32_t release_time;
    uint32_t absolute_deadline;
    uint32_t completion_time;
    uint32_t execution_time;
} dd_task;

typedef struct gen_data
{
	uint32_t execution_time;
	uint32_t relative_deadline;
	uint32_t task_id;
} gen_data;


//typedef struct node;
typedef struct node
{
    dd_task * task_ptr;
    struct node* next;
} node;

//Message Struct
typedef struct
{
    enum Request_Types req;
    node * node_ptr;
    uint32_t sender;
} message, *message_ptr;

/* Tasks */
/*****************************************************************************/

static void DD_Scheduler( void *pvParameters );
static void Task_Generator( void *pvParameters );
static void Auxiliary_Task( void *pvParameters );
static void Task_Monitor (void *pvParameters );

/* local Functions */
/*****************************************************************************/

static BaseType_t dd_create(enum Task_Types type, uint32_t task_id, uint32_t absolute_deadline, uint32_t execution_time, const char *const task_name);
static BaseType_t dd_delete(uint32_t TaskToDelete);
static BaseType_t dd_return_active_list(void );
static BaseType_t dd_return_complete_list(void );
static BaseType_t dd_return_overdue_list(void );
static BaseType_t insert_node(node** head, node* new_node );
static void adjust_priority(node* head );
static node* remove_node(node** head, uint32_t target );
static void print_list(node *head );
static void Delay_Init(void );
static void prvSetupHardware( void );
static void gpioSetup (void );
static void adcSetup(void);
static BaseType_t insert_node_unsorted(node * *head, node *new_node);
static node* allocate_node();
static BaseType_t unallocate_node(node* deleted_node);


/* Global Variables */
/*****************************************************************************/

node * head = NULL;
node * completed_head = NULL;
node * overdue_head = NULL;

uint32_t EXECUTION = 0;
uint32_t multiplier =0;
volatile uint32_t utilization=0;
TickType_t START=0; //The start time of the scheduler.
TickType_t CURRENT_SLEEP =0;
xQueueHandle xDDSQueue_handle =0;
xQueueHandle xDDSG_Queue_handle=0;
xQueueHandle deleteQueue_handle = 0;
xQueueHandle activeQueue_handle = 0;
xQueueHandle completedQueue_handle = 0;
xQueueHandle overdueQueue_handle = 0;
xQueueHandle createQueue_handle = 0;
SemaphoreHandle_t create_semaphore_handle = 0;

/*-----------------------------------------------------------*/
// You need to complete main part : int
int main(void)
{
    /*Configure the system ready to run the demo. The clock configuration
    can be done here if it was not done before main() was called*/

	prvSetupHardware();
    /*initialize the multiplier that is used to convert delay into cycle.*/
    Delay_Init();
    /* Create a Binary Semaphore for Create*/
    create_semaphore_handle = xSemaphoreCreateMutex();
    /* Create the queue used by the queue send and queue receive tasks. */
	xDDSQueue_handle = xQueueCreate(schedule_QUEUE_LENGTH, sizeof( message ) );
    xDDSG_Queue_handle = xQueueCreate(schedule_QUEUE_LENGTH, sizeof( message ) );
    createQueue_handle = xQueueCreate(message_QUEUE_LENGTH, sizeof(BaseType_t));
    /* Add to the registry, for the benefit of kernel aware debugging. */
    vQueueAddToRegistry(createQueue_handle, "CreateQueue");
	vQueueAddToRegistry( xDDSQueue_handle, "ScheduleQueue" );
	vQueueAddToRegistry( xDDSG_Queue_handle, "SchedulePeriodicQueue" );
    /* Start the tasks and timer running. */
	gen_data *gen_data1;
	gen_data1->task_id = 100;
	gen_data1->execution_time = 500;
	gen_data1->relative_deadline = 100;
	gen_data *gen_data2;
	gen_data2->task_id = 200;
	gen_data2->execution_time = 150;
	gen_data2->relative_deadline = 500;
	gen_data *gen_data3;
	gen_data3->task_id = 300;
	gen_data3->execution_time = 250;
	gen_data3->relative_deadline = 750;

    xTaskCreate( DD_Scheduler, "DeadlineDrivenSchedulerTask", configMINIMAL_STACK_SIZE, NULL, DD_SCHEDULER_PRIO, NULL);
    xTaskCreate( Task_Generator, "GeneratingTask1", configMINIMAL_STACK_SIZE, (void*)gen_data1, GENERATOR_PRIO, NULL);
//    xTaskCreate( Task_Generator, "GeneratingTask2", configMINIMAL_STACK_SIZE, (void*)gen_data2, GENERATOR_PRIO, NULL);
//    xTaskCreate( Task_Generator, "GeneratingTask3", configMINIMAL_STACK_SIZE, (void*)gen_data3, GENERATOR_PRIO, NULL);
    xTaskCreate( Task_Monitor, "MonitorTask", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    printf("Starting Scheduler!\n");
    vTaskStartScheduler();
    for( ;; )
    	printf("...\n"); // we should never get here!
    return 0;
}

/*
 * DD_Scheduler
 *
 */
static void DD_Scheduler(void *pvParameters)
{
    message schedule_message;
    node* schedule_node = NULL;
    BaseType_t response = pdFAIL;
    CURRENT_SLEEP = 100; // This is for initialization and to give the generator task to have a chance to run at the start of the program.
    START = xTaskGetTickCount();

    while (1)
    {
        /* waits to receive a scheduling request. If there is a task running, the CURRENT_SLEEP
        time is the deadline of the running task. If the scheduler does not receive anything and times
        out, it means that the task has missed the deadline. Because tasks send a delete request if they
        meet their deadline. */
        if (xQueueReceive(xDDSQueue_handle, &schedule_message, CURRENT_SLEEP))
        {
            switch (schedule_message.req)
            {
                case create:;
                    //Grab the node already allocated in the message
                	schedule_node = schedule_message.node_ptr;
                    //Insert the node into active list
                    if (insert_node(&head, schedule_node) == pdPASS)
                    {
                        //Update release time for the node
                        schedule_node->task_ptr->release_time = xTaskGetTickCount() - START;
                        response = pdPASS;
                        xQueueSend(createQueue_handle, &response, 100);
                        printf("Schedule Task!\n");
                    }else{
                    	printf("Failed to insert node");
                    }
                    break;
                case delete :
                    //Grab the node out of active list, put it into completed (no extra mem is allocated for this)
                    schedule_node = remove_node(&head, schedule_message.node_ptr->task_id);
                    if (schedule_node != NULL)
                    {
                        schedule_node->task_ptr->completion_time = xTaskGetTickCount() - START;
                        insert_node_unsorted(&completed_head, schedule_node);

                        response = pdPASS;
                        xQueueSend(deleteQueue_handle, &response, 100);
                        printf("Deleted Task!\n");
                    }
                    break;
                case active_task_list:
                    xQueueSend(activeQueue_handle, &head, 100);
                    break;
                case completed_task_list:
                    xQueueSend(completedQueue_handle, &completed_head, 100);
                    break;
                case overdue_task_list:
                    xQueueSend(overdueQueue_handle, &overdue_head, 100);
                    break;
                default :
                    printf("DDScheduler received an invalid msg!\n");
                    break;
            }
            adjust_priority(head);
        }
        else
        {
            /* Check the task_list. If it is not empty, being here means a deadline is missed!
             * the scheduler should do these:
             * lower the priority of the overdue task
             * move it to the overdue list
             * raise the priority of the next task in the ready queue
             * notifies the task generator to create the next instance of the (now overdue) task. */
            if (head != NULL)
            {
            	printf("Overdue\n");
                /* deadline is reached */
                // place task in overdue list; remove from active list
                schedule_node = remove_node(&head, head->task_ptr->task_id);
                if (schedule_node != NULL)
                {
                	insert_node_unsorted(&overdue_head, schedule_node);
                }
                // Readjust priorities
                adjust_priority(head);
                // send message to generator to create periodic task again
                // TODO: complete here else
            } else
            {
                CURRENT_SLEEP = 100;
                printf("Noting to do yet!\n");
            }
        }
    }
}
/*
 * Task Generator
 *
 */
static void Task_Generator(void *pvParameters)
{    
    uint32_t relative_deadline = ((gen_data*)pvParameters)->relative_deadline;

    //Get a new node for this task
    node* task_node = allocate_node();
    task_node->task_ptr->type = periodic; 
	task_node->task_ptr->absolute_deadline = xTaskGetTickCount() - START + relative_deadline;
	task_node->task_ptr->task_id = ((gen_data*)pvParameters)->task_id;
	task_node->task_ptr->execution_time = ((gen_data*)pvParameters)->execution_time;


    printf("Task Generator\n");
    while (1)
    {
        char task_name[10];
        sprintf(task_name, "TASK%d", task_node->task_ptr->task_id);

    	xSemaphoreTake(create_semaphore_handle,100);
    	if (dd_create(task_node , &task_name) == pdFAIL){
    	        printf("dd_tcreate Failed!\n");
    	}
    	xSemaphoreGive(create_semaphore_handle);
		
		printf("Delaying in Generator\n");
		vTaskDelay(relative_deadline);

        //Increment the deadline and task id
        task_node->task_ptr->absolute_deadline += relative_deadline;
		task_node->task_ptr->task_id ++;
    }
}

/*
 * Task Monitor
 *
 */
static void Task_Monitor(void *pvParameters)
{
    while (1)
    {
        printf("System idle time is %d\n", utilization);
        printf("ACTIVE TASKS: \n");
        dd_return_active_list();
        printf("\nCOMPLETED TASKS: \n");
        dd_return_complete_list();
        printf("\nOVERDUE TASKS: \n");
        dd_return_overdue_list();
        vTaskDelay(5000);
    }
}

/*
 * Auxillary Task
 *
 */
static void Auxiliary_Task(void *pvParameters)
{
    dd_task *task_parameters = (dd_task *)pvParameters;
    uint32_t cycles = (task_parameters->execution_time) * (23760);
    printf("Aux task start. ex time: %d Task ID: %d, abs deadline: %d\n", cycles, task_parameters->task_id, task_parameters->absolute_deadline);
//    printf("AST starting at %d\n", xTaskGetTickCount());
    while (1)
    {
    	while (cycles--){
        	//nothing
        }
        // delete the task!
//        printf("AET %d, Task ID: %d count:%d\n", xTaskGetTickCount(), task_parameters->task_id, count);
        if(dd_delete(task_parameters->task_id) == pdPASS){
        	printf("deleting task!");
            vTaskDelete(xTaskGetCurrentTaskHandle());
        }else
        	printf("Failed to delete Task!");
    }
}

/*
 * dd_create
 *
 */
static BaseType_t dd_create(node* node_data, const char *const task_name)
{
    printf("dd_tcreate. Name: %s, task id: %d\n", task_name, node_data->task_ptr->task_id);
    printf("dd_Create: execution time: %d, absolute deadline: %d\n", node_data->task_ptr->execution_time, node_data->task_ptr->absolute_deadline);
    
    BaseType_t response = pdFAIL;
    message message_create = {
    		.req = create,
			.node_ptr = node_data,
            .sender = xTaskGetCurrentTaskHandle()
    };

    //Create a new task
    //TODO: Switch this back to aux_STACK_SIZE - or use configMINIMAL_STACK_SIZE
    if (xTaskCreate(Auxiliary_Task, task_name, 200, (void*)(node_data->task_ptr), LOW_PRIO, &(node_data->task_ptr->t_handle)) == pdPASS)
    {
        //Check that the thandle is not NULL
    	if(node_data->task_ptr->t_handle == NULL){
    		printf("failed to get task handle in dd_create");
    		return pdFAIL;
    	}
        //Suspend the task until the scheduler sets the priority
        vTaskSuspend(node_data->task_ptr->t_handle);

        //Send message to Scheduler
        if (xQueueSend(xDDSQueue_handle, &message_create, portMAX_DELAY))
        {
            //Wait for the scheduler to respond
            if (xQueueReceive(createQueue_handle, &response, portMAX_DELAY))
            {
                if (response == pdPASS)
                {
                    printf("Created new Task!\n");
                    //Resume the task 
                    vTaskResume(node_data->task_ptr->t_handle);
                    return pdPASS;
                }
                else {
                	printf("response was not pdPASS\n");
                }
            }
        }
    }
    printf("Cannot Create Auxiliary task at the moment!\n");
    return pdFAIL;
}

/*
 * dd_delete
 *
 */
static BaseType_t dd_delete(uint32_t TaskToDelete)
{
    printf("dd_delete: %d\n", TaskToDelete);
    BaseType_t response = pdFAIL;
    message message_delete;
    message_delete.sender = TaskToDelete;
    message_delete.req = delete;
    message_delete.node_ptr = NULL;

    //Create queue
    deleteQueue_handle = xQueueCreate(message_QUEUE_LENGTH, sizeof(response));
    vQueueAddToRegistry(deleteQueue_handle, "CreateQueue");

    //Send message to Scheduler
    if (xQueueSend(xDDSQueue_handle, &message_delete, 100))
    {
        //Wait for response from schedule
        if (xQueueReceive(deleteQueue_handle, &response, 100))
        {
            if (response == pdPASS)
            {
                printf("Deleted a Task!\n");
                vQueueUnregisterQueue(deleteQueue_handle);
                vQueueDelete(deleteQueue_handle);
                return pdPASS;
            }
            else
                return pdFAIL;
        }
    }
    return pdFAIL;
}

/*
 * dd_return_active_list
 *
 */
static BaseType_t dd_return_active_list(void)
{
    printf("Return Active list\n");
    node *response = NULL;
    message message_active;
    message_active.req = active_task_list;
    message_active.node_ptr = NULL;
    message_active.sender = xTaskGetCurrentTaskHandle();

    activeQueue_handle = xQueueCreate(message_QUEUE_LENGTH, sizeof( message ) );
    vQueueAddToRegistry(activeQueue_handle, "ActiveListQueue");
    //no task data in message
    if (xQueueSend(xDDSQueue_handle, &message_active, 100))
    {
        if (xQueueReceive(activeQueue_handle, &response, 100))
        {
            if (response != NULL)
            {
                //active list is now the response
                print_list(response);
                return pdPASS;
            }
            else
                return pdFAIL;
        }
    }

	printf("Cannot send msg to the scheduler!\n");
	return pdFAIL;

}

/*
 * dd_return_overdue_list
 *
 */
static BaseType_t dd_return_overdue_list(void)
{
    printf("Return overdue list\n");
    node *response = NULL;
    message message_overdue;
    message_overdue.req = overdue_task_list;
    message_active.node_ptr = NULL;
    message_active.sender = xTaskGetCurrentTaskHandle();

    //Create queue
    overdueQueue_handle = xQueueCreate(message_QUEUE_LENGTH, sizeof(response));
    vQueueAddToRegistry(overdueQueue_handle, "OverdueListQueue");

    if (xQueueSend(xDDSQueue_handle, &message_overdue, 100))
    {
        if (xQueueReceive(overdueQueue_handle, &response, 100))
        {
            if (response != NULL)
            {
                print_list(response);
                vQueueUnregisterQueue(overdueQueue_handle);
                vQueueDelete(overdueQueue_handle);
                return pdPASS;
            }
            else
                return pdFAIL;
        }
    }
    else
    {
        printf("Cannot send msg to the scheduler!\n");
        return pdFAIL;
    }
    return pdPASS;
}
/*
 * dd_return_completed_list
 *
 */
static BaseType_t dd_return_complete_list(void)
{
    printf("Return completed list\n");
    node *response = NULL;
    message message_completed;
    message_completed.req = completed_task_list;
    message_active.node_ptr = NULL;
    message_active.sender = xTaskGetCurrentTaskHandle();

    //Create queue
    completedQueue_handle = xQueueCreate(message_QUEUE_LENGTH, sizeof(response));
    vQueueAddToRegistry(completedQueue_handle, "CompletedListQueue");
    if (xQueueSend(xDDSQueue_handle, &message_completed, 100))
    {
        if (xQueueReceive(completedQueue_handle, &response, 100))
        {
            if (response != NULL)
            {
                print_list(response);
                vQueueUnregisterQueue(completedQueue_handle);
                vQueueDelete(completedQueue_handle);
                return pdPASS;
            }
            else
                return pdFAIL;
        }
    }
	printf("Cannot send msg to the scheduler!\n");
	return pdFAIL;

}

/*
 * insert_node
 * Insert a task_list (in sorted order) into a (sorted) task list
 */
static BaseType_t insert_node(node **head, node * new_node)
{
	printf("Insert node: %x\n", (char*)new_node->task_ptr->t_handle);
	    node *current = *head;
	    node *previous = *head;
	    int count = 0;

	    if(*head == NULL){
	        //Empty list
	        *head = new_node;
	        (*head)->next = NULL;
	    }else if(current->task_ptr->absolute_deadline > new_node->task_ptr->absolute_deadline){
	        //New node inserted at the beginning of the list
	        //new_node->current->next->...
	        *head = new_node;
	        (*head)->next = current;
	    }else{
	    	printf("list search\n");
	        //search through list for place
	        while(current != NULL && (current->task_ptr->absolute_deadline <= new_node->task_ptr->absolute_deadline)
	        	&& count < 10){
	            previous = current;//save the last node
	            current = current->next;//increment to the next node
	            count ++;
	        }
	        if(count == 10){
	        	printf("FUCK");
	        }
	        //current node now has a later deadline than the new node
	        //head->....->previous->new_node->current
	        printf("add to list\n");
	        previous->next = new_node;
	        new_node->next = current;
	    }
	    //Head shouldn't be updated because its all pass by reference, so it's fine
	    return pdPASS;
}

/* Function to assign high priority to head of active list,
* and low priority to all other tasks in list.
* Additionally, this function modifies the 'CURRENT_SLEEP' value, which is
* the time until the next deadline.
*/
void adjust_priority(node * head)
{
    if (head == NULL)
    {
        // no task. sleep so task generator can create some tasks ...
        return;
    }
    // NOTE: at any given time, only one task (the head) should have 'high priority'
    // check if current head of list is 'high priority'
    TaskHandle_t task_handle = head->task_ptr->t_handle;
    if (uxTaskPriorityGet(task_handle) != (UBaseType_t)HIGH_PRIO)
    {
        // set head to highest priority
        vTaskPrioritySet(task_handle, (UBaseType_t)HIGH_PRIO);
        // set next element to low priority
        // Second element should always be the high priority because we update after every change
        if(head->next != NULL){
        	task_handle = head->next->task_ptr->t_handle;
			vTaskPrioritySet(task_handle, (UBaseType_t)LOW_PRIO);
        }
    }
    CURRENT_SLEEP = head->task_ptr->absolute_deadline - (xTaskGetTickCount() - START);
    printf("Adjusting priority, head: %s, sleep time: %lu\n", (char*)head->task_ptr->t_handle, CURRENT_SLEEP);
};

/* Remove a specified task_list from a task list */
static node *remove_node(node * *head, uint32_t target)
{
	 printf("Remove node: %d\n", target);
	    node *deleted_node = NULL;
	    node *current = *head;
	    
	    if (current == NULL) {
            //This is an empty list
	        deleted_node == NULL;
	    } else if (current->task_ptr->task_id == target){
            // target is head of list
	    	printf("target is the head\n");
	        deleted_node = current;
	        *head = current->next;
            //Remove this node from the list
            deleted_node->next = NULL;
	    }
	    // target is in middle of list
	    else{
	        // traverse list, looking for target
	        while (current->next != NULL){
	            if(current->next->task_ptr->task_id == target){
	                deleted_node = current->next;
	                current->next = deleted_node->next;
                    //remove this node from the list
                    deleted_node->next = NULL;
	                break;
	            }
	            current = current->next;
	        }
	    }
	    return deleted_node;
}

static BaseType_t insert_node_unsorted(node * *head, node *new_node){
	//put the new node at the end of the list
	node *current = *head;
	node *top = *head;
    uint32_t count = 0;

	if(top == NULL){
		//empty list
		top = new_node;
		top->next = NULL;
		*head = top;
		return pdPASS;
	}
    //Traverse to the end of list
	while(current->next != NULL){
		current = current->next;
        count++;
	}
	//at the end of the list
	current->next = new_node;
	new_node->next = NULL;

    //Limit list size to 10 items (delete top items)
    if(count > 10){
        node * node_to_delete = *head;
        *head = node_to_delete->next;
        node_to_delete->next = NULL;
        unallocate_node(node_to_delete);

    }

	return pdPASS;
}

/* Outputs the task list */
void print_list(node * head)
{
    node *current = head;
    // traverse the list, printing each task_list's id, execution time, and deadline
    while (current != NULL)
    {
        printf("task Aux, exec_time = %d, deadline = %d, response time = %d\n",
                current->task_ptr-> execution_time, current->task_ptr->absolute_deadline, current->task_ptr->completion_time - current->task_ptr->release_time);
        current = (node *)current->next;
    }
}

/*-----------------------------------------------------------*/
static void Delay_Init(void)
{
    RCC_ClocksTypeDef RCC_Clocks;
    /* Get system clocks */
    RCC_GetClocksFreq(&RCC_Clocks);
    /* While loop takes 4 cycles */
    /* For 1 ms delay, we need to divide with 4K */
    printf("Freq: %d \n", RCC_Clocks.HCLK_Frequency);
    multiplier = RCC_Clocks.HCLK_Frequency / Mul_Divider; // to calculate 1 msecfmonitor
    printf("mult: %d \n", multiplier);
}
/*-----------------------------------------------------------*/
void vApplicationMallocFailedHook(void)
{
    /* The malloc failed hook is enabled by setting
    configUSE_MALLOC_FAILED_HOOK to 1 in FreeRTOSConfig.h.
    Called if a call to pvPortMalloc() fails because there is insufficient
    free memory available in the FreeRTOS heap. pvPortMalloc() is called
    internally by FreeRTOS API functions that create tasks, queues, software
    timers, and semaphores. The size of the FreeRTOS heap is set by the
    configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
    for (;;)
        ;
}
/*-----------------------------------------------------------*/
void vApplicationStackOverflowHook(xTaskHandle pxTask, signed char
                                                            *pcTaskName)
{
    (void)pcTaskName;
    (void)pxTask;
    /* Run time stack overflow checking is performed if
    configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook
    function is called if a stack overflow is detected. pxCurrentTCB can be
    inspected in the debugger if the task name passed into this function is
    corrupt. */
    for (;;)
        ;
}
/*-----------------------------------------------------------*/
void vApplicationIdleHook(void)
{
    utilization++;
}
/*-----------------------------------------------------------*/
static void prvSetupHardware(void)
{
    /* Ensure all priority bits are assigned as preemption priority bits.
    http://www.freertos.org/RTOS-Cortex-M3-M4.html */
    NVIC_SetPriorityGrouping(0);
    utilization++;
    /* TODO: Setup the clocks, etc. here, if they were not configured before
    main() was called. */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
	gpioSetup();
	adcSetup();
}
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

static node* allocate_node(){
    node* new_node = (node*)pvPortMalloc(sizeof(node));
    //Make sure there is mem available
    configASSERT(new_node);
    //Set default values for node
    new_node->next = NULL;
    new_node->task_ptr->type = periodic;
    new_node->task_ptr->t_handle = NULL;
    new_node->task_ptr->task_id = 0;
    new_node->task_ptr->release_time = 0;
    new_node->task_ptr->absolute_deadline = 0;
    new_node->task_ptr->completion_time = 0;
    new_node->task_ptr->execution_time = 0;
    return new_node;
}

static BaseType_t unallocate_node(node* deleted_node){
    //Make sure the node isn't NULL, and isnt' connected to a list
    if(deleted_node != NULL && deleted_node->next == NULL){
        
        deleted_node->task_ptr->t_handle = NULL;
        deleted_node->task_ptr->task_id = 0;
        deleted_node->task_ptr->type = periodic;
        deleted_node->task_ptr->release_time = 0;
        deleted_node->task_ptr->absolute_deadline = 0;
        deleted_node->task_ptr->completion_time = 0;
        deleted_node->task_ptr->execution_time = 0;

        vPortFree((void*)deleted_node)
        return pdPASS;
    }
    return pdFAIL;

}