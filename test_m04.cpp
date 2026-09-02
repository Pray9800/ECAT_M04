#include <stdio.h>
#include <string.h>

// SOEM是C库，需要用extern "C"包含
#ifdef __cplusplus
extern "C" {
#endif

#include "soem/include/soem/soem.h"

#ifdef __cplusplus
}
#endif

#define EC_TIMEOUTMON 500

// ============================================================
// 网卡配置 - 修改这里填入你的网卡GUID
// ============================================================
// 使用 'ipconfig /all' 查看你的网卡GUID
// 格式: \\Device\\NPF_{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
#define DEFAULT_ADAPTER "\\Device\\NPF_{10665CCF-F790-4087-A630-9BD2F614C3EA}"
// ============================================================

static ecx_contextt ctx;
static uint8 IOmap[4096];

// 打印从站信息
void printSlaveInfo(ecx_contextt *context, int slave_num) {
    printf("\n=== Slave %d Info ===\n", slave_num);
    printf("Name: %s\n", context->slavelist[slave_num].name);
    printf("Vendor ID: 0x%08X\n", context->slavelist[slave_num].eep_man);
    printf("Product Code: 0x%08X\n", context->slavelist[slave_num].eep_id);
    printf("Revision: 0x%08X\n", context->slavelist[slave_num].eep_rev);
    printf("Serial: 0x%08X\n", context->slavelist[slave_num].eep_ser);
    printf("DL Address: 0x%04X\n", context->slavelist[slave_num].configadr);
    printf("State: 0x%02X\n", context->slavelist[slave_num].state);
    printf("Input PDO Size: %d bytes\n", context->slavelist[slave_num].Ibytes);
    printf("Output PDO Size: %d bytes\n", context->slavelist[slave_num].Obytes);
    
    // 检查是否是M04设备 (AMSAMOTION_EC 厂商ID: 0xB95)
    if (context->slavelist[slave_num].eep_man == 0xB95) {
        printf(">>> This is AMSAMOTION device!\n");
        if (context->slavelist[slave_num].eep_id == 0x00001410) {
            printf(">>> This is EC2-MB-M04 device!\n");
        }
    }
}

int main(int argc, char *argv[]) {
    int cnt;
    int expected;
    int chk;
    const char* adapterName;
    
    printf("=== M04 EtherCAT Device Test ===\n");
    printf("================================\n\n");
    
    // 确定使用的网卡
    if (argc >= 2) {
        // 使用命令行参数指定的网卡
        adapterName = argv[1];
        printf("Using adapter from command line\n");
    } else {
        // 使用默认网卡
        adapterName = DEFAULT_ADAPTER;
        printf("Using default adapter configuration\n");
        printf("To modify, edit DEFAULT_ADAPTER in test_m04.cpp\n");
    }
    
    printf("Adapter: %s\n", adapterName);
    printf("\n");
    
    // 初始化SOEM
    printf("Initializing SOEM...\n");
    if (ecx_init(&ctx, adapterName)) {
        printf("SOEM initialization succeeded!\n");
        
        // 扫描从站
        printf("\nScanning EtherCAT slaves...\n");
        if (ecx_config_init(&ctx) > 0) {
            printf("Found %d slave(s)\n", ctx.slavecount);
            
            if (ctx.slavecount > 0) {
                // 打印所有从站信息
                for (cnt = 1; cnt <= ctx.slavecount; cnt++) {
                    printSlaveInfo(&ctx, cnt);
                }
                
                // 配置PDO映射
                printf("\nConfiguring PDO mapping...\n");
                ecx_config_map_group(&ctx, IOmap, 0);
                
                // 配置分布式时钟
                printf("Configuring distributed clocks...\n");
                ecx_configdc(&ctx);
                
                // 等待所有从站达到SAFE-OP状态
                printf("\nWaiting for slaves to enter SAFE-OP state...\n");
                expected = (ctx.grouplist[0].outputsWKC * 2) +
                              ctx.grouplist[0].inputsWKC;
                
                // 请求SAFE-OP状态
                ctx.slavelist[0].state = EC_STATE_SAFE_OP;
                ecx_writestate(&ctx, 0);
                chk = 200;
                do {
                    ecx_send_processdata(&ctx);
                    ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
                    ecx_statecheck(&ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
                } while (chk-- && (ctx.slavelist[0].state != EC_STATE_SAFE_OP));
                
                if (ctx.slavelist[0].state == EC_STATE_SAFE_OP) {
                    printf("Slaves entered SAFE-OP state!\n");
                    
                    // 请求OP状态
                    printf("\nRequesting OP state...\n");
                    ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
                    ecx_writestate(&ctx, 0);
                    chk = 200;
                    do {
                        ecx_send_processdata(&ctx);
                        ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
                        ecx_statecheck(&ctx, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);
                    } while (chk-- && (ctx.slavelist[0].state != EC_STATE_OPERATIONAL));
                    
                    if (ctx.slavelist[0].state == EC_STATE_OPERATIONAL) {
                        printf("Slaves entered OP state!\n");
                        printf("\n=== Communication test succeeded! ===\n");
                        
                        // 保持运行一段时间进行测试
                        printf("\nMaintaining communication for 10 seconds...\n");
                        for (int i = 0; i < 1000; i++) {
                            ecx_send_processdata(&ctx);
                            ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
                            osal_usleep(10000); // 10ms
                            
                            if (i % 100 == 0) {
                                printf("  Test progress: %d%%\n", i / 10);
                            }
                        }
                        printf("  Test progress: 100%%\n");
                        
                    } else {
                        printf("Failed to enter OP state!\n");
                        printf("Current state: 0x%02X\n", ctx.slavelist[0].state);
                    }
                } else {
                    printf("Failed to enter SAFE-OP state!\n");
                    printf("Current state: 0x%02X\n", ctx.slavelist[0].state);
                }
                
                // 打印最终状态
                printf("\n=== Final Slave Status ===\n");
                for (cnt = 1; cnt <= ctx.slavecount; cnt++) {
                    printf("Slave %d: State=0x%02X, Lost=%d\n", 
                           cnt, 
                           ctx.slavelist[cnt].state,
                           ctx.slavelist[cnt].islost);
                }
            }
        } else {
            printf("No EtherCAT slaves found!\n");
            printf("\nPossible reasons:\n");
            printf("1. Network cable not connected\n");
            printf("2. M04 device not powered on\n");
            printf("3. Wrong adapter name\n");
            printf("4. Npcap not installed\n");
            printf("5. Insufficient privileges (requires administrator)\n");
        }
        
        // 关闭SOEM
        printf("\nClosing SOEM...\n");
        ecx_close(&ctx);
        printf("SOEM closed\n");
        
    } else {
        printf("SOEM initialization failed!\n");
        printf("\nPossible reasons:\n");
        printf("1. Wrong adapter name\n");
        printf("2. Npcap not installed\n");
        printf("3. Insufficient privileges (requires administrator)\n");
        printf("4. Adapter in use by another program\n");
    }
    
    printf("\n=== Test Complete ===\n");
    printf("Press Enter to exit...\n");
    getchar();
    
    return 0;
}
