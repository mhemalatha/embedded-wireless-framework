/**************************************************************************/
/*                                                                        */
/*       Copyright (c) Microsoft Corporation. All rights reserved.        */
/*                                                                        */
/*       This software is licensed under the Microsoft Software License   */
/*       Terms for Microsoft Azure RTOS. Full text of the license can be  */
/*       found in the LICENSE file at https://aka.ms/AzureRTOS_EULA       */
/*       and in the root directory of this software.                      */
/*                                                                        */
/**************************************************************************/

#include "ewf_allocator_threadx.h"
#include "ewf_interface_win32_com.h"
#include "ewf_adapter_espressif_wroom_02.h"
#include "ewf_lib.h"
#include "ewf_middleware_netxduo.h"
#include "ewf_example.config.h"

#define SAMPLE_DHCP_DISABLE

#include <time.h>

#include "nx_api.h"
#ifndef SAMPLE_DHCP_DISABLE
#include "nxd_dhcp_client.h"
#endif /* SAMPLE_DHCP_DISABLE */
#include "nxd_dns.h"
#include "nx_secure_tls_api.h"
#include "nxd_sntp_client.h"

/* Include the sample.  */
extern VOID sample_entry(NX_IP* ip_ptr, NX_PACKET_POOL* pool_ptr, NX_DNS* dns_ptr, UINT (*unix_time_callback)(ULONG *unix_time));

/* Define the helper thread for running Azure SDK on ThreadX (THREADX IoT Platform).  */
#ifndef SAMPLE_HELPER_STACK_SIZE
#define SAMPLE_HELPER_STACK_SIZE        (4096)
#endif /* SAMPLE_HELPER_STACK_SIZE  */

#ifndef SAMPLE_HELPER_THREAD_PRIORITY
#define SAMPLE_HELPER_THREAD_PRIORITY   (4)
#endif /* SAMPLE_HELPER_THREAD_PRIORITY  */

/* Define user configurable symbols. */
#ifndef SAMPLE_IP_STACK_SIZE
#define SAMPLE_IP_STACK_SIZE            (2048)
#endif /* SAMPLE_IP_STACK_SIZE  */

#ifndef SAMPLE_PACKET_COUNT
#define SAMPLE_PACKET_COUNT             (32)
#endif /* SAMPLE_PACKET_COUNT  */

#ifndef SAMPLE_PACKET_SIZE
#define SAMPLE_PACKET_SIZE              (1536)
#endif /* SAMPLE_PACKET_SIZE  */

#define SAMPLE_POOL_SIZE                ((SAMPLE_PACKET_SIZE + sizeof(NX_PACKET)) * SAMPLE_PACKET_COUNT)

#ifndef SAMPLE_ARP_CACHE_SIZE
#define SAMPLE_ARP_CACHE_SIZE           (512)
#endif /* SAMPLE_ARP_CACHE_SIZE  */

#ifndef SAMPLE_IP_THREAD_PRIORITY
#define SAMPLE_IP_THREAD_PRIORITY       (1)
#endif /* SAMPLE_IP_THREAD_PRIORITY */

#ifndef SAMPLE_SNTP_SYNC_MAX
#define SAMPLE_SNTP_SYNC_MAX             (30)
#endif /* SAMPLE_SNTP_SYNC_MAX */

#ifndef SAMPLE_SNTP_UPDATE_MAX
#define SAMPLE_SNTP_UPDATE_MAX            (30)
#endif /* SAMPLE_SNTP_UPDATE_MAX */

#ifndef SAMPLE_SNTP_UPDATE_INTERVAL
#define SAMPLE_SNTP_UPDATE_INTERVAL       (NX_IP_PERIODIC_RATE / 2)
#endif /* SAMPLE_SNTP_UPDATE_INTERVAL */

/* Default time. GMT: Friday, Jan 1, 2022 12:00:00 AM. Epoch timestamp: 1640995200.  */
#ifndef SAMPLE_SYSTEM_TIME
#define SAMPLE_SYSTEM_TIME                1640995200
#endif /* SAMPLE_SYSTEM_TIME  */

/* Seconds between Unix Epoch (1/1/1970) and NTP Epoch (1/1/1999) */
#define SAMPLE_UNIX_TO_NTP_EPOCH_SECOND   (0x83AA7E80)

ULONG g_ip_address = 0;
ULONG g_network_mask = 0;
ULONG g_gateway_address = 0;
ULONG g_dns_address = 0;

static TX_THREAD        sample_helper_thread;
static NX_PACKET_POOL   pool_0;
static NX_IP            ip_0;
static NX_DNS           dns_0;
#ifndef SAMPLE_DHCP_DISABLE
static NX_DHCP          dhcp_0;
#endif /* SAMPLE_DHCP_DISABLE  */
/* SNTP Instance */
static NX_SNTP_CLIENT   sntp_0;

/* Define the stack/cache for ThreadX.  */
static ULONG sample_ip_stack[SAMPLE_IP_STACK_SIZE / sizeof(ULONG)];
#ifndef SAMPLE_POOL_STACK_USER
static ULONG sample_pool_stack[SAMPLE_POOL_SIZE / sizeof(ULONG)];
static ULONG sample_pool_stack_size = sizeof(sample_pool_stack);
#else
extern ULONG sample_pool_stack[];
extern ULONG sample_pool_stack_size;
#endif
static ULONG sample_arp_cache_area[SAMPLE_ARP_CACHE_SIZE / sizeof(ULONG)];
static ULONG sample_helper_thread_stack[SAMPLE_HELPER_STACK_SIZE / sizeof(ULONG)];

/* Define the prototypes for sample thread.  */
static void sample_helper_thread_entry(ULONG parameter);

#ifndef SAMPLE_DHCP_DISABLE
static void dhcp_wait();
#endif /* SAMPLE_DHCP_DISABLE */

static UINT dns_create();

static ULONG unix_time_base;
static UINT unix_time_get(ULONG *unix_time);
static UINT sntp_time_sync();

static const CHAR* sntp_servers[] =
{
  "0.pool.ntp.org",
  "1.pool.ntp.org",
  "2.pool.ntp.org",
  "3.pool.ntp.org",
};
static UINT sntp_server_index;

/* Define main entry point.  */
int main(void)
{

    /* Enter the ThreadX kernel.  */
    tx_kernel_enter();
}

/* Define what the initial system looks like.  */
void    tx_application_define(void *first_unused_memory)
{

UINT  status;


    NX_PARAMETER_NOT_USED(first_unused_memory);


    /* Create sample helper thread. */
    status = tx_thread_create(&sample_helper_thread, "Demo Thread",
                              sample_helper_thread_entry, 0,
                              sample_helper_thread_stack, SAMPLE_HELPER_STACK_SIZE,
                              SAMPLE_HELPER_THREAD_PRIORITY, SAMPLE_HELPER_THREAD_PRIORITY,
                              TX_NO_TIME_SLICE, TX_AUTO_START);

    /* Check status.  */
    if (status)
    {
        printf("Demo helper thread creation fail: %u\r\n", status);
        return;
    }
}

/* Define sample helper thread entry.  */
void sample_helper_thread_entry(ULONG parameter)
{

UINT status;

ULONG ip_address = 0;
ULONG network_mask = 0;
ULONG gateway_address = 0;

ewf_result result;

ewf_allocator* message_allocator_ptr = NULL;
ewf_interface* interface_ptr = NULL;
ewf_adapter* adapter_ptr = NULL;


    NX_PARAMETER_NOT_USED(parameter);


    EWF_ALLOCATOR_THREADX_STATIC_DECLARE(message_allocator_ptr, message_allocator,
        EWF_CONFIG_MESSAGE_ALLOCATOR_BLOCK_COUNT,
        EWF_CONFIG_MESSAGE_ALLOCATOR_BLOCK_SIZE);
    EWF_INTERFACE_WIN32_COM_STATIC_DECLARE(interface_ptr, com_port,
        EWF_CONFIG_INTERFACE_WIN32_COM_PORT_FILE_NAME,
        EWF_CONFIG_INTERFACE_WIN32_COM_PORT_BAUD_RATE,
        EWF_CONFIG_INTERFACE_WIN32_COM_PORT_BYTE_SIZE,
        EWF_CONFIG_INTERFACE_WIN32_COM_PORT_PARITY,
        EWF_CONFIG_INTERFACE_WIN32_COM_PORT_STOP_BITS);
    EWF_ADAPTER_ESPRESSIF_WROOM_02_STATIC_DECLARE(adapter_ptr, wroom02, message_allocator_ptr, NULL, interface_ptr);

    // Start the adapter
    if (ewf_result_failed(result = ewf_adapter_start(adapter_ptr)))
    {
        EWF_LOG_ERROR("Failed to start the adapter, ewf_result %d.\n", result);
        exit(result);
    }

    // Set the SIM PIN
    if (ewf_result_failed(result = ewf_adapter_modem_sim_pin_enter(adapter_ptr, EWF_CONFIG_SIM_PIN)))
    {
        EWF_LOG_ERROR("Failed to the SIM PIN, ewf_result %d.\n", result);
        exit(result);
    }

    // Enable full functionality
    if (ewf_result_failed(result = ewf_adapter_modem_functionality_set(adapter_ptr, EWF_ADAPTER_MODEM_FUNCTIONALITY_FULL)))
    {
        EWF_LOG_ERROR("Failed to set the ME functionality, ewf_result %d.\n", result);
        exit(result);
    }

    if (ewf_result_failed(result = ewf_adapter_get_ipv4_address(adapter_ptr, (uint32_t*)&g_ip_address)))
    {
        EWF_LOG_ERROR("Failed to get the adapter IPv4 address: ewf_result %d.\n", result);
        exit(result);
    }

    if (ewf_result_failed(result = ewf_adapter_get_ipv4_netmask(adapter_ptr, (uint32_t*)&g_network_mask)))
    {
        EWF_LOG_ERROR("Failed to get the adapter IPv4 netmask: ewf_result %d.\n", result);
        exit(result);
    }

    if (ewf_result_failed(result = ewf_adapter_get_ipv4_gateway(adapter_ptr, (uint32_t*)&g_gateway_address)))
    {
        EWF_LOG_ERROR("Failed to get the adapter IPv4 gateway: ewf_result %d.\n", result);
        exit(result);
    }

    if (ewf_result_failed(result = ewf_adapter_get_ipv4_dns(adapter_ptr, (uint32_t*)&g_dns_address)))
    {
        EWF_LOG_ERROR("Failed to get the adapter IPv4 DNS: ewf_result %d.\n", result);
        exit(result);
    }

    /* Initialize the NetX system.  */
    nx_system_initialize();

    /* Create a packet pool.  */
    status = nx_packet_pool_create(&pool_0, "NetX Main Packet Pool", SAMPLE_PACKET_SIZE,
                                   (UCHAR*)sample_pool_stack, sample_pool_stack_size);

    /* Check for pool creation error.  */
    if (status)
    {
        printf("nx_packet_pool_create fail: %u\r\n", status);
        return;
    }

    /* Create an IP instance.  */
    status = nx_ip_create(&ip_0, "NetX IP Instance 0",
                          g_ip_address, g_network_mask,
                          &pool_0,
                          nx_driver_ewf_adapter,
                          (UCHAR*)sample_ip_stack, sizeof(sample_ip_stack),
                          SAMPLE_IP_THREAD_PRIORITY);

    /* Check for IP create errors.  */
    if (status)
    {
        printf("nx_ip_create fail: %u\r\n", status);
        return;
    }

    /* Save the adapter pointer in the IP instance */
    ip_0.nx_ip_interface->nx_interface_additional_link_info = adapter_ptr;

    /* Enable ARP and supply ARP cache memory for IP Instance 0.  */
    status = nx_arp_enable(&ip_0, (VOID*)sample_arp_cache_area, sizeof(sample_arp_cache_area));

    /* Check for ARP enable errors.  */
    if (status)
    {
        printf("nx_arp_enable fail: %u\r\n", status);
        return;
    }

    /* Enable ICMP traffic.  */
    status = nx_icmp_enable(&ip_0);

    /* Check for ICMP enable errors.  */
    if (status)
    {
        printf("nx_icmp_enable fail: %u\r\n", status);
        return;
    }

    /* Enable TCP traffic.  */
    status = nx_tcp_enable(&ip_0);

    /* Check for TCP enable errors.  */
    if (status)
    {
        printf("nx_tcp_enable fail: %u\r\n", status);
        return;
    }

    /* Enable UDP traffic.  */
    status = nx_udp_enable(&ip_0);

    /* Check for UDP enable errors.  */
    if (status)
    {
        printf("nx_udp_enable fail: %u\r\n", status);
        return;
    }

    /* Initialize TLS.  */
    nx_secure_tls_initialize();

#ifndef SAMPLE_DHCP_DISABLE
    dhcp_wait();
#else
    nx_ip_gateway_address_set(&ip_0, g_gateway_address);
#endif /* SAMPLE_DHCP_DISABLE  */

    /* Get IP address and gateway address. */
    nx_ip_address_get(&ip_0, &ip_address, &network_mask);
    nx_ip_gateway_address_get(&ip_0, &gateway_address);

    /* Output IP address and gateway address. */
    printf("IP address: %lu.%lu.%lu.%lu\r\n",
           (ip_address >> 24),
           (ip_address >> 16 & 0xFF),
           (ip_address >> 8 & 0xFF),
           (ip_address & 0xFF));
    printf("Mask: %lu.%lu.%lu.%lu\r\n",
           (network_mask >> 24),
           (network_mask >> 16 & 0xFF),
           (network_mask >> 8 & 0xFF),
           (network_mask & 0xFF));
    printf("Gateway: %lu.%lu.%lu.%lu\r\n",
           (gateway_address >> 24),
           (gateway_address >> 16 & 0xFF),
           (gateway_address >> 8 & 0xFF),
           (gateway_address & 0xFF));

    /* Create DNS.  */
    status = dns_create();

    /* Check for DNS create errors.  */
    if (status)
    {
        printf("dns_create fail: %u\r\n", status);
        return;
    }

    /* Sync up time by SNTP at start up. */
    status = sntp_time_sync();

    /* Check status.  */
    if (status)
    {
        printf("SNTP Time Sync failed.\r\n");

#if 1
        printf("Using the time() function if available.\n");
        unix_time_base = (ULONG)time(NULL);
#else
        printf("Set Time to default value: SAMPLE_SYSTEM_TIME.\n");
        unix_time_base = SAMPLE_SYSTEM_TIME;
#endif

    }
    else
    {
        printf("SNTP Time Sync successfully.\r\n");
    }

    ULONG unix_time = 0;

    /* Use time to init the seed.  */
    unix_time_get(&unix_time);

    /* Use time to init the seed. FIXME: use real rand on device.  */
    srand((unsigned int)time(NULL));

    /* Start sample.  */
    sample_entry(&ip_0, &pool_0, &dns_0, unix_time_get);
}

#ifndef SAMPLE_DHCP_DISABLE
static void dhcp_wait()
{
ULONG   actual_status;

    printf("DHCP In Progress...\r\n");

    /* Create the DHCP instance.  */
    nx_dhcp_create(&dhcp_0, &ip_0, "DHCP Client");

    /* Start the DHCP Client.  */
    nx_dhcp_start(&dhcp_0);

    /* Wait util address is solved. */
    nx_ip_status_check(&ip_0, NX_IP_ADDRESS_RESOLVED, &actual_status, NX_WAIT_FOREVER);
}
#endif /* SAMPLE_DHCP_DISABLE  */

static UINT dns_create()
{

UINT    status;
ULONG   dns_server_address[3];
UINT    dns_server_address_size = 12;

    /* Create a DNS instance for the Client.  Note this function will create
       the DNS Client packet pool for creating DNS message packets intended
       for querying its DNS server. */
    status = nx_dns_create(&dns_0, &ip_0, (UCHAR *)"DNS Client");
    if (status)
    {
        return(status);
    }

    /* Is the DNS client configured for the host application to create the packet pool? */
#ifdef NX_DNS_CLIENT_USER_CREATE_PACKET_POOL

    /* Yes, use the packet pool created above which has appropriate payload size
       for DNS messages. */
    status = nx_dns_packet_pool_set(&dns_0, ip_0.nx_ip_default_packet_pool);
    if (status)
    {
        nx_dns_delete(&dns_0);
        return(status);
    }
#endif /* NX_DNS_CLIENT_USER_CREATE_PACKET_POOL */

#ifndef SAMPLE_DHCP_DISABLE
    /* Retrieve DNS server address.  */
    nx_dhcp_interface_user_option_retrieve(&dhcp_0, 0, NX_DHCP_OPTION_DNS_SVR, (UCHAR *)(dns_server_address),
                                           &dns_server_address_size);
#else
    dns_server_address[0] = g_dns_address;
#endif /* SAMPLE_DHCP_DISABLE */

    /* Add an IPv4 server address to the Client list. */
    status = nx_dns_server_add(&dns_0, dns_server_address[0]);
    if (status)
    {
        nx_dns_delete(&dns_0);
        return(status);
    }

    /* Output DNS Server address.  */
    printf("DNS Server address: %lu.%lu.%lu.%lu\r\n",
           (dns_server_address[0] >> 24),
           (dns_server_address[0] >> 16 & 0xFF),
           (dns_server_address[0] >> 8 & 0xFF),
           (dns_server_address[0] & 0xFF));

    return(NX_SUCCESS);
}

/* Sync up the local time. */
static UINT sntp_time_sync_internal(ULONG sntp_server_address)
{
    UINT status;
    UINT server_status;
    UINT i;

    /* Create the SNTP Client to run in broadcast mode.. */
    status = nx_sntp_client_create(&sntp_0, &ip_0, 0, &pool_0, NX_NULL, NX_NULL,
        NX_NULL /* no random_number_generator callback */);

    /* Check status. */
    if (status == NX_SUCCESS)
    {
        /* Use the IPv4 service to initialize the Client and set the IPv4 SNTP server. */
        status = nx_sntp_client_initialize_unicast(&sntp_0, sntp_server_address);

        /* Check status. */
        if (status != NX_SUCCESS)
        {
            nx_sntp_client_delete(&sntp_0);
            return (status);
        }

        /* Set local time to 0 */
        status = nx_sntp_client_set_local_time(&sntp_0, 0, 0);

        /* Check status. */
        if (status != NX_SUCCESS)
        {
            nx_sntp_client_delete(&sntp_0);
            return (status);
        }

        /* Run Unicast client */
        status = nx_sntp_client_run_unicast(&sntp_0);

        /* Check status. */
        if (status != NX_SUCCESS)
        {
            nx_sntp_client_stop(&sntp_0);
            nx_sntp_client_delete(&sntp_0);
            return (status);
        }

        /* Wait till updates are received */
        for (i = 0U; i < SAMPLE_SNTP_UPDATE_MAX; i++)
        {
            /* First verify we have a valid SNTP service running. */
            status = nx_sntp_client_receiving_updates(&sntp_0, &server_status);

            /* Check status. */
            if ((status == NX_SUCCESS) && (server_status == NX_TRUE))
            {
                /* Server status is good. Now get the Client local time. */
                ULONG sntp_seconds;
                ULONG sntp_fraction;
                ULONG system_time_in_second;

                /* Get the local time. */
                status = nx_sntp_client_get_local_time(&sntp_0, &sntp_seconds, &sntp_fraction, NX_NULL);

                /* Check status. */
                if (status != NX_SUCCESS)
                {
                    continue;
                }

                /* Get the system time in second. */
                system_time_in_second = tx_time_get() / TX_TIMER_TICKS_PER_SECOND;

                /* Convert to Unix epoch and minus the current system time. */
                unix_time_base = (sntp_seconds - (system_time_in_second + SAMPLE_UNIX_TO_NTP_EPOCH_SECOND));

                /* Time sync successfully. */

                /* Stop and delete SNTP. */
                nx_sntp_client_stop(&sntp_0);
                nx_sntp_client_delete(&sntp_0);

                return (NX_SUCCESS);
            }

            /* Sleep.  */
            tx_thread_sleep(SAMPLE_SNTP_UPDATE_INTERVAL);
        }

        /* Time sync failed. - Return not success. */
        status = NX_NOT_SUCCESSFUL;
        /* Stop and delete SNTP. */
        nx_sntp_client_stop(&sntp_0);
        nx_sntp_client_delete(&sntp_0);
    }

    return (status);
}

/* Sync up the local time.  */
static UINT sntp_time_sync(void)
{
    UINT  status;
    ULONG gateway_address;
    ULONG sntp_server_address[3];

    /* Sync time by SNTP server array. */
    for (UINT i = 0U; i < SAMPLE_SNTP_SYNC_MAX; i++)
    {
        printf("SNTP Time Sync...%s\r\n", sntp_servers[sntp_server_index]);

        /* Make sure the network is still valid. */
        while (nx_ip_gateway_address_get(&ip_0, &gateway_address))
        {
            tx_thread_sleep(NX_IP_PERIODIC_RATE);
        }

        /* Look up SNTP Server address. */
        status = nx_dns_host_by_name_get(&dns_0, (UCHAR*)sntp_servers[sntp_server_index], &sntp_server_address[0],
            (5 * NX_IP_PERIODIC_RATE));

        /* Check status. */
        if (status == NX_SUCCESS)
        {
            /* Start SNTP to sync the local time. */
            status = sntp_time_sync_internal(sntp_server_address[0]);

            /* Check status.  */
            if (status == NX_SUCCESS)
            {
                return (NX_SUCCESS);
            }
        }

        /* Switch SNTP server every time.  */
        sntp_server_index = (sntp_server_index + 1) % (sizeof(sntp_servers) / sizeof(sntp_servers[0]));
    }

    return (NX_NOT_SUCCESSFUL);
}

static UINT unix_time_get(ULONG* unix_time)
{
    /* Return number of seconds since Unix Epoch (1/1/1970 00:00:00).  */
    *unix_time = unix_time_base + (tx_time_get() / TX_TIMER_TICKS_PER_SECOND);

    return(NX_SUCCESS);
}
