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
#define LOW_PRIO 2
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
    dd_task * task_ptr;
} message, *message_ptr;

/* Tasks */
/*****************************************************************************/

static void DD_Scheduler( void *pvParameters );
static void Task_Generator( void *pvParameters );
static void Auxiliary_Task( void *pvParameters );
static void Task_Monitor (void *pvParameters );

/* local Functions */
/*****************************************************************************/

static BaseType_t dd_create(dd_task  task_data, const char *const task_name);
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

/*-----------------------------------------------------------*/
// You need to complete main part : int
int main(void)
{
    /*Configure the system ready to run the demo. The clock configuration
    can be done here if it was not done before main() was called*/
    prvSetupHardware();
    /*initialize the multiplier that is used to convert delay into cycle.*/
    Delay_Init();
    /* Create the queue used by the queue send and queue receive tasks. */
	xDDSQueue_handle = xQueueCreate(schedule_QUEUE_LENGTH, sizeof( message ) );
    xDDSG_Queue_handle = xQueueCreate(schedule_QUEUE_LENGTH, sizeof( message ) );
    createQueue_handle = xQueueCreate(message_QUEUE_LENGTH, sizeof(BaseType_t));
    /* Add to the registry, for the benefit of kernel aware debugging. */
    vQueueAddToRegistry(createQueue_handle, "CreateQueue");
	vQueueAddToRegistry( xDDSQueue_handle, "ScheduleQueue" );
	vQueueAddToRegistry( xDDSG_Queue_handle, "SchedulePeriodicQueue" );
    /* Start the tasks and timer running. */
    xTaskCreate( DD_Scheduler, "DeadlineDrivenSchedulerTask", configMINIMAL_STACK_SIZE, NULL, DD_SCHEDULER_PRIO, NULL);
    xTaskCreate( Task_Generator, "TaskGeneratingTask", configMINIMAL_STACK_SIZE, NULL, GENERATOR_PRIO, NULL);
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
    BaseType_t response = pdFAIL;
    CURRENT_SLEEP = 100; // This is for initialization and to give the generator task to have a chance to run at the start of the program.
//    node deleted = NULL;
    node *deleted = pvPortMalloc(sizeof(node*));
	deleted->task_ptr = NULL;

    // node *head_add = head;
    // message gen_msg;
    // node gen_data;
    // gen_msg.DATA = &gen_data;
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
                	node *new_node = pvPortMalloc(sizeof(node*));
					new_node->task_ptr = schedule_message.task_ptr;
					new_node->next = NULL;
                    if (insert_node(&head, new_node) == pdPASS)
                    {
                        schedule_message.task_ptr->release_time = xTaskGetTickCount() - START;
                        response = pdPASS;
                        xQueueSend(createQueue_handle, &response, 100);
                        printf("Schedule Task!\n");
                    }
                    break;
                case delete :
                    deleted = remove_node(&head, schedule_message.task_ptr->task_id);
                    if (deleted != NULL)
                    {
                        deleted->task_ptr->completion_time = xTaskGetTickCount() - START;
                        //TODO: completed head shouldn't be a sorted list, just an incremental list
//                        insert_node(&completed_head, deleted);
                        insert_node_unsorted(&completed_head, deleted);

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
                /* deadline is reached */
                // place task in overdue list; remove from active list
                deleted = remove_node(&head, head->task_ptr->task_id);
                if (deleted != NULL)
                {
                    insert_node(&overdue_head, deleted);
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
//    dd_task *pTaskParameters = pvPortMalloc(sizeof(dd_task));
	int relative_deadline = 2000;
	int absolute_deadline = xTaskGetTickCount() - START + relative_deadline;
	int current_task = 0;
	dd_task pTaskParameters = {.type = periodic,
							   .task_id = 0,
							   .execution_time = 1000,
							   .absolute_deadline = absolute_deadline,
							   .release_time = 0,
							   .completion_time = 0,
							   .t_handle = NULL };

//    message *periodic_task_message = pvPortMalloc(sizeof(message));
    message periodic_task_message = { .req = create,
    								  .task_ptr = &pTaskParameters};

//    periodic_task_message->task_ptr = pTaskParameters;
//    pTaskParameters->type = periodic;
//    pTaskParameters->execution_time = 1000;
//    pTaskParameters->absolute_deadline = xTaskGetTickCount() - START + relative_deadline;
    printf("Task Generator\n");
    if (dd_create(pTaskParameters, "TASK1") == pdFAIL)
        printf("dd_tcreate Failed!\n");
    absolute_deadline += relative_deadline;
    while (1)
    {
//        if (xQueueReceive(xDDSG_Queue_handle, &periodic_task_message, portMAX_DELAY) == pdPASS)
//        {
            // we only re-create periodic tasks
            if (periodic_task_message.task_ptr->type == periodic)
            {
            	current_task ++;
            	dd_task newpTaskParameters = {.type = periodic,
            								  .task_id = current_task,
            								   .execution_time = 1000,
            								   .absolute_deadline = absolute_deadline,
            								   .release_time = 0,
            								   .completion_time = 0,
            								   .t_handle = NULL };
            	periodic_task_message.task_ptr = &newpTaskParameters;
                // calculate deadline of next periodic task
                // = previous deadline + relative deadline

                int startTime = pdMS_TO_TICKS(absolute_deadline-relative_deadline);
                int currentTime = xTaskGetTickCount() - START;
                printf("start time: %d, current time: %d\n", startTime, currentTime);

//                pTaskParameters.absolute_deadline += relative_deadline;
                // calculate time until task should be created (its period)
                if (startTime > currentTime)
                {
                	printf("Delaying in Generator\n");
                    vTaskDelay(startTime - currentTime);
                }

                // Create task
                printf("Starting Task at: %d, with start time %d\n", xTaskGetTickCount() - START, startTime);
                if (dd_create(newpTaskParameters, "PERIODIC_TASK") == pdFAIL)
                    printf("dd_tcreate Failed!\n");
                absolute_deadline += relative_deadline;
            }
//        }
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
        printf("System idle time is %lu\n", utilization);
        printf("ACTIVE TASKS: \n");
        dd_return_active_list();
        printf("\nCOMPLETED TASKS: \n");
        dd_return_complete_list();
        printf("\nOVERDUE TASKS: \n");
        dd_return_overdue_list();
        vTaskDelay(10000);
    }
}

/*
 * Auxillary Task
 *
 */
static void Auxiliary_Task(void *pvParameters)
{
    dd_task *task_parameters = (dd_task *)pvParameters;
    uint32_t cycles = (task_parameters->execution_time);
    printf("Aux task start. ex time: %d\n", cycles);

    printf("AST starting at %d\n", xTaskGetTickCount());
    while (1)
    {
    	TickType_t task_start = xTaskGetTickCount();
        while (xTaskGetTickCount() > task_start+cycles){
        	//nothing
        }
        // delete the task!
        printf("AET %d\n", xTaskGetTickCount());
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
static BaseType_t dd_create(dd_task  task_data, const char *const task_name)
{
    printf("dd_tcreate. Name: %s, task id: %d\n", task_name, task_data.task_id);
    BaseType_t response = pdFAIL;
    message message_create;//message
    TaskHandle_t Task_thandle = NULL;//task handle
    message_create.req = create;
    message_create.task_ptr = &task_data;
    //return queue to get response from scheduler
//    createQueue_handle = xQueueCreate(message_QUEUE_LENGTH, sizeof(response));
//    vQueueAddToRegistry(createQueue_handle, "CreateQueue");

    //Create a new task
    //TODO: Switch this back to aux_STACK_SIZE
    if (xTaskCreate(Auxiliary_Task, task_name, 100, &task_data, 1,&Task_thandle) == pdPASS)
    {
        //Add new task to the message
    	if(Task_thandle != NULL){
    		message_create.task_ptr->t_handle = Task_thandle;
    	} else {
    		printf("failed to get task handle in dd_create");
    		return pdFAIL;
    	}

        //Send message to Scheduler
        if (xQueueSend(xDDSQueue_handle, &message_create, 100))
        {
            //Wait for the scheduler to respond
            if (xQueueReceive(createQueue_handle, &response, 100))
            {
                if ((BaseType_t)response == pdPASS)
                {
                    printf("Created new Task!\n");
//                    vQueueUnregisterQueue(createQueue_handle);
//                    vQueueDelete(createQueue_handle);
                }
                else {
                	printf("response was not pdPASS\n");
                    return pdFAIL;
                }
            }
        }
        else return pdFAIL;
    }
    else
    {
        printf("Cannot Create Auxiliary task at the moment!\n");
        return pdFAIL;
    }
    return pdPASS;
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
    dd_task deleteTask;
    deleteTask.task_id = TaskToDelete;
    message_delete.task_ptr = &deleteTask;
    message_delete.req = delete;

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
	        //search through list for place
	        while(current->task_ptr->absolute_deadline < new_node->task_ptr->absolute_deadline){
	            previous = current;//save the last node
	            current = current->next;//increment to the next node
	        }
	        //current node now has a later deadline than the new node
	        //head->....->previous->new_node->current
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
//	printf("Adjust Priority\n");
//    UBaseType_t priority;
//	while(1);
    if (head == NULL)
    {
        // no task. sleep so task generator can create some tasks ...
//    	printf("No Tasks!\n");
        return;
    }
    // NOTE: at any given time, only one task (the head) should have 'high priority'
    // check if current head of list is 'high priority'
    TaskHandle_t task_handle = head->task_ptr->t_handle;
    if (uxTaskPriorityGet(task_handle) != (UBaseType_t)HIGH_PRIO)
    {
//    	printf("Head != NULL\n");
        // set head to highest priority
        vTaskPrioritySet(task_handle, (UBaseType_t)HIGH_PRIO);
        // set next element to low priority
        // Second element should always be the high priority because we update after every change
        if(head->next != NULL){
//        	printf("Head->next is not NULL\n");
        	task_handle = head->next->task_ptr->t_handle;
			vTaskPrioritySet(task_handle, (UBaseType_t)LOW_PRIO);
        }
    }
//    printf("before sleep\n");
    CURRENT_SLEEP = head->task_ptr->absolute_deadline - (xTaskGetTickCount() - START);
    printf("Adjusting priority, head: %s, sleep time: %lu\n", (char*)head->task_ptr->t_handle, CURRENT_SLEEP);
};

/* Remove a specified task_list from a task list */
static node *remove_node(node * *head, uint32_t target)
{
	 printf("Remove node: %x\n", (int)target);
	    node *deleted_node = NULL;
	    node *current = *head;
	    // target is head of list
	    if (current == NULL) {
	        return current;
	    } else if (current->task_ptr->task_id == target)
	    {
	    	printf("target is the head\n");
	        deleted_node = current;
	        *head = current->next;
	    }
	    // target is in middle of list
	    else
	    {
	        // traverse list, looking for target
	        while (current->next != NULL){
	            if(current->next->task_ptr->task_id == target){
	                deleted_node = current->next;
	                current->next = deleted_node->next;
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

	if(top == NULL){
		//empty list
		top = new_node;
		top->next = NULL;
		*head = top;
		return pdPASS;
	}
	//TODO: REMOVE
	while(1);
	while(current->next != NULL){
		current = current->next;
	}

	//at the end of the list
	current->next = new_node;
	new_node->next = NULL;

	return pdPASS;
}

/* Outputs the task list */
void print_list(node * head)
{
    node *current = head;
    // traverse the list, printing each task_list's id, execution time, and deadline
    while (current != NULL)
    {
        printf("task Aux, exec_time = %d, deadline = %d\n",
                current->task_ptr-> execution_time, current->task_ptr->absolute_deadline);
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
    printf("Freq: %lu \n", RCC_Clocks.HCLK_Frequency);
    multiplier = RCC_Clocks.HCLK_Frequency / Mul_Divider; // to calculate 1 msecfmonitor
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
