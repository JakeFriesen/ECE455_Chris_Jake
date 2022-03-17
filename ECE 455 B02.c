/*ECE 455 Project 2 - Deadline Driven Scheduler
    Chris Dunn
    Jake Friesen
*/
/* Standard includes */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stm32f4_discovery.h"
/* Kernel includes */ .
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
    enum Task_Types type; 
    uint32_t release_time;
    uint32_t absolute_deadline; 
    uint32_t completion_time; 
    uint32_t execution_time;
} dd_task;

typedef struct node;
typedef struct node
{
    dd_task * task_ptr;
    node* next;
} node;

//Message Struct
typedef struct
{
    enum Request_Types req
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

static TaskHandle_t dd_tcreate(dd_task* auxtParameter, const char * const task_name);
static BaseType_t dd_delete(TaskHandle_t TaskToDelet );
static BaseType_t dd_return_active_list(void );
static BaseType_t dd_return_complete_list(void );
static BaseType_t dd_return_overdue_list(void );
static BaseType_t insert_node(node** head, node* new_node );
static void adjust_priority(node* head );
static node* remove_node(node** head, TaskHandle_t target );
static void print_list(node *head );
static void Delay_Init(void );
static void prvSetupHardware( void );

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
xQueueHandle creatQueue_handle = 0;

/*-----------------------------------------------------------*/
// You need to complete main part : int
main(void)
{
    /*Configure the system ready to run the demo. The clock configuration
    can be done here if it was not done before main() was called*/
    prvSetupHardware();
    /*initialize the multiplier that is used to convert delay into cycle.*/
    Delay_Init();
    /* Create the queue used by the queue send and queue receive tasks. */
	xDDSQueue_handle = xQueueCreate(schedule_QUEUE_LENGTH, sizeof( message ) );
    xDDSG_Queue_handle = xQueueCreate(schedule_QUEUE_LENGTH, sizeof( message ) );
    /* Add to the registry, for the benefit of kernel aware debugging. */
	vQueueAddToRegistry( xDDSQueue_handle, "ScheduleQueue" );
	vQueueAddToRegistry( xDDSG_Queue_handle, "SchedulePeriodicQueue" );
    /* Start the tasks and timer running. */
    xTaskCreate( DD_Scheduler, "DeadlineDrivenSchedulerTask", configMINIMAL_STACK_SIZE, NULL, DD_SCHEDULER_PRIO, NULL);
    xTaskCreate( Task_Generator, "TaskGeneratingTask", configMINIMAL_STACK_SIZE, NULL, GENERATOR_PRIO, NULL);
    xTaskCreate( Task_Monitor, "MonitorTask", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    vTaskStartScheduler();
    for( ;; ); // we should never get here!
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
    node deleted = NULL;
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
                case create:
                    
                    if (insert_node(&head, schedule_message.task_ptr) == pdPASS)
                    {
                        schedule_message.task_ptr->release_time = xTaskGetTickCount() - START;
                        xQueueSend(creatQueue, (BaseType_t*)pdPASS, 100);
                        printf("Schedule Task!");
                    }
                    break;
                case delete : 
                    deleted = remove_node(&head, schedule_message.task_ptr->t_handle);
                    if (deleted != NULL)
                    {
                        deleted.task_ptr->completion_time = xTaskGetTickCount() - START;
                        insert_node(&completed_head, deleted.task_ptr);
                        xQueueSend(deleteQueue, (BaseType_t*)pdPASS, 100);
                        printf("Deleted Task!");
                    }
                    break;
                case return_active_list:
                    xQueueSend(activeQueue_handle, &head, 100)
                    break;
                case return_completed_list:
                    xQueueSend(completedQueue_handle, &completed_head, 100)
                    break;
                case return_overdue_list:
                    xQueueSend(overdueQueue_handle, &overdue_head, 100)
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
                deleted = remove_node(&head, &head.task_ptr->t_handle);
                if (deleted != NULL)
                {
                    insert_node(&overdue_head, deleted.task_ptr);
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
/*
 * Task Generator
 *
 */
static void Task_Generator(void *pvParameters)
{
    dd_task pTaskParameters;
    message periodic_task_message;
    int relative_deadline = 2000;
    periodic_task_message.DATA = &regen_DATA;
    pTaskParameters.type = periodic;
    pTaskParameters.execution_time = 1000;
    pTaskParameters.absolute_deadline = xTaskGetTickCount() - START + relative_deadline;
    
    if (dd_tcreate(&pTaskParameters, "TASK1") == NULL)
        printf("dd_tcreate Failed!\n");
    while (1)
    {
        if (xQueueReceive(xDDSG_Queue_handle, &periodic_task_message, portMAX_DELAY) == pdPASS)
        {
            // we only re-create periodic tasks
            if (periodic_task_message.DATA->type == periodic)
            {
                // calculate deadline of next periodic task
                // = previous deadline + relative deadline
                
                int startTime = pTaskParameters.absolute_deadline;
                int currentTime = xTaskGetTickCount() - START;

                pTaskParameters.absolute_deadline += relative_deadline;
                // calculate time until task should be created (its period)
                if (startTime < currentTime) 
                {
                    vTaskDelay(startTime - currentTime);
                }
                
                // Create task
                if (dd_tcreate(&pTaskParameters, "PERIODIC_TASK") == NULL)
                    printf("dd_tcreate Failed!\n");
            }
        }
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
    printf("Aux task start. ex time: %d", task_parameters->execution_time);
    uint32_t *cycles = &(task_parameters->execution_time);
    printf("AST starting at %d\n", xTaskGetTickCount());
    while (1)
    {
        while ((*cycles)--)
            ;
        // delete the task!
        printf("AET %d\n", xTaskGetTickCount());
        if(dd_delete(xTaskGetCurrentTaskHandle()) == pd_pass){
            vTaskDelete(xTaskGetCurrentTaskHandle());
        }
    }
}

/*
 * dd_create
 *
 */
static TaskHandle_t dd_tcreate(dd_task * task_data, const char *const task_name)
{
    printf("dd_tcreate. Name: %s, handle: %s", task_name, task_date->t_handle);
    BaseType_t response = pdFAIL;
    message message_create;//message
    TaskHandle_t Task_thandle = NULL;//task handle
    message_create.req = create;
    message_create.task_ptr = task_data;
    //return queue to get response from scheduler
    creatQueue_handle = xQueueCreate(message_QUEUE_LENGTH, sizeof(response));
    vQueueAddToRegistry(creatQueue_handle, "CreateQueue");
    
    //Create a new task
    if (xTaskCreate(Auxiliary_Task, task_name, Aux_STACK_SIZE, task_data, 1,&Task_thandle) == pdPASS)
    {
        //Add new task to the message
        message_create.task_ptr.t_handle = &Task_thandle;
        //Send message to Scheduler
        if (xQueueSend(xDDSQueue_handle, &message_create, 100))
        {
            //Wait for the scheduler to respond
            if (xQueueReceive(creatQueue_handle, &response, 100))
            {
                if (response == pdPASS)
                {
                    printf("Created new Task!");
                    vQueueUnregisterQueue(creatQueue_handle);
                    vQueueDelete(creatQueue_handle);
                }
                else
                    return NULL;
            }
        }
        else return NULL;
    }
    else
    {
        printf("Cannot Create Auxiliary task at the moment!\n");
        return NULL;
    }
}

/*
 * dd_delete
 *
 */
static BaseType_t dd_delete(TaskHandle_t TaskToDelete)
{
    printf("dd_delete: %s", TaskToDelete);
    BaseType_t response = pdFAIL;
    message message_delete;
    dd_task deleteTask;
    deleteTask.t_handle = TaskToDelete;
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
                printf("Deleted a Task!");
                vQueueUnregisterQueue(deleteQueue_handle);
                vQueueDelete(deleteQueue_handle);
                return pdPASS;
            }
            else
                return pdFAIL;
        }
    }
    else return pdFAIL;
}

/*
 * dd_return_active_list
 *
 */
static BaseType_t dd_return_active_list(void)
{
    printf("Return Active list");
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
    else
    {
        printf("Cannot send msg to the scheduler!\n");
        return pdFAIL;
    }
}

/*
 * dd_return_overdue_list
 *
 */
static BaseType_t dd_return_overdue_list(void)
{
    printf("Return overdue list");
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
}
/*
 * dd_return_completed_list
 *
 */
static BaseType_t dd_return_completed_list(void)
{
    printf("Return completed list");
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
    else
    {
        printf("Cannot send msg to the scheduler!\n");
        return pdFAIL;
    }
}

/*
 * insert_node
 * Insert a task_list (in sorted order) into a (sorted) task list
 */
static BaseType_t insert_node(node **head, node * new_node)
{
    printf("Insert node: %s", new_node.task_ptr->t_handle);
    new_node->next = NULL;
    node *current = *head;
    node *old_node = (node *)current->next;
    // list is empty or new_node is new head
    if(current == NULL || current->next->task_ptr->absolute_deadline < new_node->absolute_deadline){
        current = new_node;
        new_node->next = old_node;
    }else{
        //search for place to put node
        while(current->next != NULL){
            if(current->next->task_ptr->absolute_deadline < new_node->absolute_deadline){
                //put new node here
                old_node = current->next;
                current->next = new_node;
                new_node->next = old_node;
                break;
            }
            current = current->next;
        }
        //put task at end of list
        current->next = new_node;
    }
    //update head
    *head = current;
    return pdPASS;
}

/* Function to assign high priority to head of active list,
* and low priority to all other tasks in list.
* Additionally, this function modifies the 'CURRENT_SLEEP' value, which is
* the time until the next deadline.
*/
void adjust_priority(node * head)
{
    UBaseType_t priority;
    if (head == NULL)
    {
        // no task. sleep so task generator can create some tasks ...
        return;
    }
    // NOTE: at any given time, only one task (the head) should have 'high priority'
    // check if current head of list is 'high priority'
    TaskHandle_t task_handle = head.task_ptr->t_handle;
    if (uxTaskPriorityGet(task_handle) != (UBaseType_t)HIGH_PRIO)
    {
        // set head to highest priority
        vTaskPrioritySet(task_handle, (UBaseType_t)HIGH_PRIO);
        // set next element to low priority
        // Second element should always be the high priority because we update after every change
        task_handle = head.next.task_ptr->t_handle;
        vTaskPrioritySet(task_handle, (UBaseType_t)LOW_PRIO);
    }
    CURRENT_SLEEP = head.task_ptr->absolute_deadline - (xTaskGetTickCount() - START);
    printf("Adjusting priority, head: %s, sleep time: %d", head.task_ptr->t_handle, CURRENT_SLEEP);
};

/* Remove a specified task_list from a task list */
static node *remove_node(node * *head, TaskHandle_t target)
{
    printf("Remove node: %s", target);
    node *deleted_node = NULL;
    node *current = *head;
    // target is head of list
    if ((*head)->task_ptr->t_handle == target)
    {
        current = current->next;
        *head = current;
    }
    // target is in middle of list
    else
    {
        // traverse list, looking for target
        while (current->next != NULL){
            if(current->next->task_ptr->t_handle == target){
                deleted_node = current->next;
                temp_node = (node *)current->next->next;
                current->next = temp_node;
            }
            current = current->next;
        }
    }
    return deleted_node;
}

/* Outputs the task list */
void print_list(node * head)
{
    node *current = head;
    // traverse the list, printing each task_list's id, execution time, and deadline
    while (current != NULL)
    {
        printf("task Aux, exec_time = %ld, deadline = %ld\n",
                current -> execution_time, current->deadline);
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
}