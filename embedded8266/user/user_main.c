//Copyright 2015 <>< Charles Lohr Under the MIT/x11 License, NewBSD License or
// ColorChord License.  You Choose.

/*============================================================================
 * Includes
 *==========================================================================*/

#include "mem.h"
#include "c_types.h"
#include "user_interface.h"
#include "ets_sys.h"
#include "uart.h"
#include "osapi.h"
#include "espconn.h"
#include "esp82xxutil.h"
#include "ws2812_i2s.h"
#include "hpatimer.h"
#include "DFT32.h"
#include "ccconfig.h"
#include <embeddednf.h>
#include <embeddedout.h>
#include <commonservices.h>
#include "ets_sys.h"
#include "gpio.h"
#include "custom_commands.h"

/*============================================================================
 * Defines
 *==========================================================================*/

//#define PROFILE

#define PORT 7777
#define SERVER_TIMEOUT 1500
#define MAX_CONNS 5
#define MAX_FRAME 2000

#define PROC_TASK_PRIO        0
#define PROC_TASK_QUEUE_LEN    1

/*============================================================================
 * Variables
 *==========================================================================*/

struct CCSettings CCS = {0};
static os_timer_t some_timer = {0};
static struct espconn *pUdpServer = NULL;

static bool hpa_is_paused_for_wifi = false;

os_event_t    procTaskQueue[PROC_TASK_QUEUE_LEN] = {0};
uint32_t samp_iir = 0;
int samplesProcessed = 0;

extern volatile uint8_t sounddata[HPABUFFSIZE];
extern volatile uint16_t soundhead;
uint16_t soundtail;

/*============================================================================
 * Functions
 *==========================================================================*/

void EnterCritical();
void ExitCritical();

/**
 * Called from procTask() when 128 sound samples, a colorchord frame,
 * have been received and filtered.
 *
 * This updates the LEDs.
 */
static void NewFrame()
{
	if( !COLORCHORD_ACTIVE )
	{
		return;
	}

	int i;
	HandleFrameInfo();

	switch( COLORCHORD_OUTPUT_DRIVER )
	{
	case 0:
		UpdateLinearLEDs();
		break;
	case 1:
		UpdateAllSameLEDs();
		break;
	};

	ws2812_push( ledOut, USE_NUM_LIN_LEDS * 3, LED_DRIVER_MODE );
}

void fpm_wakup_cb_func1(void) { wifi_fpm_close(); }

/**
 * The main process that runs all the time. Registered with system_os_task(),
 * priority 0 (PROC_TASK_PRIO). Always posts an event to itself to stay running.
 *
 * This task filters samples stored in the sounddata[] queue and feeds them into PushSample32()
 *
 * When enough samples have been filtered, it'll call NewFrame() to change the LEDs.
 *
 * If no samples are queued for filtering, call CSTick()
 *
 * @param events OS signals, sent through system_os_post(). If sig an par
 *               are 0, call CSTick()
 */
static void procTask(os_event_t *events)
{
	// TODO why this instead of a while(1) loop?
	system_os_post(PROC_TASK_PRIO, 0, 0 );

	if( COLORCHORD_ACTIVE && !isHpaRunning() )
	{
		ExitCritical();
	}

	if( !COLORCHORD_ACTIVE && isHpaRunning() )
	{
		EnterCritical();
	}
	
	//For profiling so we can see how much CPU is spent in this loop.
#ifdef PROFILE
	WRITE_PERI_REG( PERIPHS_GPIO_BASEADDR + GPIO_ID_PIN(0), 1 );
#endif
	while( sampleAvailable() )
	{
		int32_t samp = getSample();
		samp_iir = samp_iir - (samp_iir>>10) + samp;
		samp = (samp - (samp_iir>>10))*16;
		samp = (samp * CCS.gINITIAL_AMP) >> 4;
		PushSample32( samp );

		samplesProcessed++;
		if( samplesProcessed == 128 )
		{
			NewFrame();
			samplesProcessed = 0; 
		}
	}
#ifdef PROFILE
	WRITE_PERI_REG( PERIPHS_GPIO_BASEADDR + GPIO_ID_PIN(0), 0 );
#endif

	if( events->sig == 0 && events->par == 0 )
	{
		CSTick( 0 );
	}
}

/**
 * Timer handler for a software timer set to fire every 100ms, forever.
 * Calls CSTick() every 100ms.
 * If the hardware is in wifi station mode, this Enables the hardware timer
 * to sample the ADC once the IP address has been received and printed
 *
 * @param arg unused
 */
static void ICACHE_FLASH_ATTR myTimer(void *arg)
{
	CSTick( 1 );

	if( GPIO_INPUT_GET( 0 ) == 0 )
	{
		if( system_get_rst_info()->reason != 5 ) //See if we're booting from deep sleep.  (5 = deep sleep, 4 = reboot, 0 = fresh boot)
		{
			system_deep_sleep_set_option(4);	//Option 4 = When rebooting from deep sleep, totally disable wifi.
			system_deep_sleep(2000);
		}
		else
		{
			//system_deep_sleep_set_option(1);	//Option 1 = Reboot with radio recalibration and wifi on.
			//system_deep_sleep(2000);
			system_restart();
		}
	}

	if( hpa_is_paused_for_wifi && printed_ip )
	{
		StartHPATimer(); //Init the high speed  ADC timer.
		hpa_is_paused_for_wifi = false; // only need to do once prevents unstable ADC
	}
//	uart0_sendStr(".");
//	printf( "%d/%d\n",soundtail,soundhead );
//	printf( "%d/%d\n",soundtail,soundhead );
//	uint8_t ledout[] = { 0x00, 0xff, 0xaa, 0x00, 0xff, 0xaa, };
//	ws2812_push( ledout, 6, LED_DRIVER_MODE );
}


/**
 * UDP Packet handler, registered with espconn_regist_recvcb().
 * It takes the UDP data and jams it right into the LEDs
 *
 * @param arg The espconn struct for this packet
 * @param pusrdata
 * @param len
 */
static void ICACHE_FLASH_ATTR udpserver_recv(void *arg, char *pusrdata, unsigned short len)
{
	struct espconn *pespconn = (struct espconn *)arg;
//	uint8_t buffer[MAX_FRAME];
//	uint8_t ledout[] = { 0x00, 0xff, 0xaa, 0x00, 0xff, 0xaa, };
	uart0_sendStr("X");
	ws2812_push( pusrdata+3, len, LED_DRIVER_MODE );
}

/**
 * UART RX handler, called by uart0_rx_intr_handler(). Currently does nothing
 *
 * @param c The char received on the UART
 */
void ICACHE_FLASH_ATTR charrx( uint8_t c )
{
	//Called from UART.
}

/**
 * The main initialization function
 */
void ICACHE_FLASH_ATTR user_init(void)
{
	uart_init(BIT_RATE_115200, BIT_RATE_115200);
	int wifiMode = wifi_get_opmode();

	uart0_sendStr("\r\nColorChord\r\n");

	//Uncomment this to force a system restore.
	//	system_restore();

	// Load configurable parameters from SPI memory
	CustomStart();

#ifdef PROFILE
	GPIO_OUTPUT_SET(GPIO_ID_PIN(0), 0);
#endif

	//Tricky: New recommendation is to connect GPIO14 to vcc for audio circuitry, so we turn this on by default.
	GPIO_OUTPUT_SET( GPIO_ID_PIN(14), 1);
	PIN_FUNC_SELECT( PERIPHS_IO_MUX_MTMS_U, 3 );

	// Common services (wifi) pre-init.
	CSPreInit();

	// Set up UDP server to receive LED data, udpserver_recv()
    pUdpServer = (struct espconn *)os_zalloc(sizeof(struct espconn));
	ets_memset( pUdpServer, 0, sizeof( struct espconn ) );
	espconn_create( pUdpServer );
	pUdpServer->type = ESPCONN_UDP;
	pUdpServer->proto.udp = (esp_udp *)os_zalloc(sizeof(esp_udp));
	pUdpServer->proto.udp->local_port = 7777;
	espconn_regist_recvcb(pUdpServer, udpserver_recv);

	if( espconn_create( pUdpServer ) )
	{
		while(1) { uart0_sendStr( "\r\nFAULT\r\n" ); }
	}

	// Common services (wifi) init. Sets up another UDP server to receive
	// commands (issue_command)and an HTTP server
	CSInit( 1 );

	// Add a process to filter queued ADC samples and output LED signals
	system_os_task(procTask, PROC_TASK_PRIO, procTaskQueue, PROC_TASK_QUEUE_LEN);

	// Start a software timer to call CSTick() every 100ms and start the hw timer eventually
	os_timer_disarm(&some_timer);
	os_timer_setfn(&some_timer, (os_timer_func_t *)myTimer, NULL);
	os_timer_arm(&some_timer, 100, 1);

	//Set GPIO16 for Input
#if 0
    WRITE_PERI_REG(PAD_XPD_DCDC_CONF,
                   (READ_PERI_REG(PAD_XPD_DCDC_CONF) & 0xffffffbc) | (uint32)0x1); 	// mux configuration for XPD_DCDC and rtc_gpio0 connection

    WRITE_PERI_REG(RTC_GPIO_CONF,
                   (READ_PERI_REG(RTC_GPIO_CONF) & (uint32)0xfffffffe) | (uint32)0x0);	//mux configuration for out enable

    WRITE_PERI_REG(RTC_GPIO_ENABLE,
                   READ_PERI_REG(RTC_GPIO_ENABLE) & (uint32)0xfffffffe);	//out disable
#endif

	system_update_cpu_freq(SYS_CPU_160MHZ);

    // Init colorchord
	InitColorChord();

	// Tricky: If we are in station mode, wait for that to get resolved before enabling the high speed timer.
	if( wifi_get_opmode() == 1 )
	{
		hpa_is_paused_for_wifi = true;
	}
	else
	{
		// Init the high speed  ADC timer.
		StartHPATimer();
	}

	// Initialize LEDs
	ws2812_init();

	printf( "RST REASON: %d\n", system_get_rst_info()->reason );

	// Attempt to make ADC more stable
	// https://github.com/esp8266/Arduino/issues/2070
	// see peripherals https://espressif.com/en/support/explore/faq
	wifi_set_sleep_type(MODEM_SLEEP_T); // on its own stopped wifi working?
	//wifi_fpm_set_sleep_type(NONE_SLEEP_T); // with this seemed no difference

	// Post an event to procTask() to get it started
	system_os_post(PROC_TASK_PRIO, 0, 0 );
}

/**
 * If the firmware leaves a critical section, enable the hardware timer
 * used to sample the ADC. This allows the interrupt to fire.
 */
void ICACHE_FLASH_ATTR EnterCritical()
{
	PauseHPATimer();
	//ets_intr_lock();
}

/**
 * If the firmware enters a critical section, disable the hardware timer
 * used to sample the ADC and the corresponding interrupt
 */
void ICACHE_FLASH_ATTR ExitCritical()
{
	//ets_intr_unlock();
	ContinueHPATimer();
}





/*==============================================================================
 * Partition Map Data
 *============================================================================*/

#define SYSTEM_PARTITION_OTA_SIZE_OPT2                 0x6A000
#define SYSTEM_PARTITION_OTA_2_ADDR_OPT2               0x81000
#define SYSTEM_PARTITION_RF_CAL_ADDR_OPT2              0xfb000
#define SYSTEM_PARTITION_PHY_DATA_ADDR_OPT2            0xfc000
#define SYSTEM_PARTITION_SYSTEM_PARAMETER_ADDR_OPT2    0xfd000
#define SYSTEM_PARTITION_CUSTOMER_PRIV_PARAM_ADDR_OPT2 0x7c000
#define SPI_FLASH_SIZE_MAP_OPT2                        2

#define SYSTEM_PARTITION_OTA_SIZE_OPT3                 0x6A000
#define SYSTEM_PARTITION_OTA_2_ADDR_OPT3               0x81000
#define SYSTEM_PARTITION_RF_CAL_ADDR_OPT3              0x1fb000
#define SYSTEM_PARTITION_PHY_DATA_ADDR_OPT3            0x1fc000
#define SYSTEM_PARTITION_SYSTEM_PARAMETER_ADDR_OPT3    0x1fd000
#define SYSTEM_PARTITION_CUSTOMER_PRIV_PARAM_ADDR_OPT3 0x7c000
#define SPI_FLASH_SIZE_MAP_OPT3                        3

#define SYSTEM_PARTITION_OTA_SIZE_OPT4                 0x6A000
#define SYSTEM_PARTITION_OTA_2_ADDR_OPT4               0x81000
#define SYSTEM_PARTITION_RF_CAL_ADDR_OPT4              0x3fb000
#define SYSTEM_PARTITION_PHY_DATA_ADDR_OPT4            0x3fc000
#define SYSTEM_PARTITION_SYSTEM_PARAMETER_ADDR_OPT4    0x3fd000
#define SYSTEM_PARTITION_CUSTOMER_PRIV_PARAM_ADDR_OPT4 0x7c000
#define SPI_FLASH_SIZE_MAP_OPT4                        4

#define SYSTEM_PARTITION_CUSTOMER_PRIV_PARAM SYSTEM_PARTITION_CUSTOMER_BEGIN
#define EAGLE_FLASH_BIN_ADDR                 SYSTEM_PARTITION_CUSTOMER_BEGIN + 1
#define EAGLE_IROM0TEXT_BIN_ADDR             SYSTEM_PARTITION_CUSTOMER_BEGIN + 2

static const partition_item_t partition_table_opt2[] =
{
    { EAGLE_FLASH_BIN_ADDR,              0x00000,                                     0x10000},
    { EAGLE_IROM0TEXT_BIN_ADDR,          0x10000,                                     0x60000},
    { SYSTEM_PARTITION_RF_CAL,           SYSTEM_PARTITION_RF_CAL_ADDR_OPT2,           0x1000},
    { SYSTEM_PARTITION_PHY_DATA,         SYSTEM_PARTITION_PHY_DATA_ADDR_OPT2,         0x1000},
    { SYSTEM_PARTITION_SYSTEM_PARAMETER, SYSTEM_PARTITION_SYSTEM_PARAMETER_ADDR_OPT2, 0x3000},
};

static const partition_item_t partition_table_opt3[] =
{
    { EAGLE_FLASH_BIN_ADDR,              0x00000,                                     0x10000},
    { EAGLE_IROM0TEXT_BIN_ADDR,          0x10000,                                     0x60000},
    { SYSTEM_PARTITION_RF_CAL,           SYSTEM_PARTITION_RF_CAL_ADDR_OPT3,           0x1000},
    { SYSTEM_PARTITION_PHY_DATA,         SYSTEM_PARTITION_PHY_DATA_ADDR_OPT3,         0x1000},
    { SYSTEM_PARTITION_SYSTEM_PARAMETER, SYSTEM_PARTITION_SYSTEM_PARAMETER_ADDR_OPT3, 0x3000},
};

static const partition_item_t partition_table_opt4[] =
{
    { EAGLE_FLASH_BIN_ADDR,              0x00000,                                     0x10000},
    { EAGLE_IROM0TEXT_BIN_ADDR,          0x10000,                                     0x60000},
    { SYSTEM_PARTITION_RF_CAL,           SYSTEM_PARTITION_RF_CAL_ADDR_OPT4,           0x1000},
    { SYSTEM_PARTITION_PHY_DATA,         SYSTEM_PARTITION_PHY_DATA_ADDR_OPT4,         0x1000},
    { SYSTEM_PARTITION_SYSTEM_PARAMETER, SYSTEM_PARTITION_SYSTEM_PARAMETER_ADDR_OPT4, 0x3000},
};



/**
 * Required. Users can call system_phy_set_rfoption to set the RF option in user_rf_pre_init,
 * or call system_deep_sleep_set_option before Deep-sleep. If RF is disabled,
 * ESP8266 Station and SoftAP will both be disabled, so the related APIs must not be
 * called. Wi-Fi radio functions and network stack management APIs are not available
 * when the radio is disabled
 *
 * This is called on boot for versions ESP8266_NONOS_SDK_v1.5.2 to
 * ESP8266_NONOS_SDK_v2.2.1. system_phy_set_rfoption() may be called here
 */
void user_rf_pre_init(void)
{
	; // nothing
}

/**
 * Required function as of ESP8266_NONOS_SDK_v3.0.0. Must call
 * system_partition_table_regist(). This tries to register a few different
 * partition maps. The ESP should be happy with one of them.
 */
void ICACHE_FLASH_ATTR user_pre_init(void)
{
    if(system_partition_table_regist(
                partition_table_opt2,
                sizeof(partition_table_opt2) / sizeof(partition_table_opt2[0]),
                SPI_FLASH_SIZE_MAP_OPT2))
    {
        os_printf("system_partition_table_regist 2 success!!\r\n");
    }
    else if(system_partition_table_regist(
                partition_table_opt4,
                sizeof(partition_table_opt4) / sizeof(partition_table_opt4[0]),
                SPI_FLASH_SIZE_MAP_OPT4))
    {
        os_printf("system_partition_table_regist 4 success!!\r\n");
    }
    else if(system_partition_table_regist(
                partition_table_opt3,
                sizeof(partition_table_opt3) / sizeof(partition_table_opt3[0]),
                SPI_FLASH_SIZE_MAP_OPT3))
    {
        os_printf("system_partition_table_regist 3 success!!\r\n");
    }
    else
    {
        os_printf("system_partition_table_regist all fail\r\n");
        while(1);
    }
}

