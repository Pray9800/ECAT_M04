//C语言基本的两个库
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

#define EC_TIMEOUTMON 500  //检查网线有没有掉 暂时没用到

// ============================================================
// 透传发送节奏参数（主循环每拍 = 10ms）
// TX_INTERVAL_TICKS : 两次发送的间隔（拍数）→ 200拍 = 2秒发一次
// TX_PULSE_TICKS    : DataValid 高电平保持拍数 → 3拍 = 30ms
// 脉冲宽度必须 ≥ 2拍(20ms)：M04 内部轮询周期约 15ms，
// DataValid 拉高的时间要覆盖至少一个 M04 轮询周期，
// ============================================================
#define TX_INTERVAL_TICKS 200
#define TX_PULSE_TICKS    5

// ============================================================
// 网卡配置
// ============================================================
#define DEFAULT_ADAPTER "\\Device\\NPF_{10665CCF-F790-4087-A630-9BD2F614C3EA}"
// ============================================================

static ecx_contextt ctx;  // SOEM 上下文对象  官方结构体
static uint8 IOmap[4096];  //存储

// 打印从站信息   数据来源是XML 也就是写在EEPROM里面的
void printSlaveInfo(ecx_contextt* context, int slave_num) {
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

    if (context->slavelist[slave_num].eep_man == 0xB95) {
        printf(">>> This is AMSAMOTION device!\n");
        if (context->slavelist[slave_num].eep_id == 0x00001410) {
            printf(">>> This is EC2-MB-M04 device!\n");
        }
    }
}
//上文 if：0xB95=AMSAMOTION 厂商、0x1410=M04 产品码，双命中就打印"EC2-MB-M04 found"， 身份确认

int main(int argc, char* argv[]) {
    int cnt;
    int expected;
    int chk;
    const char* adapterName;

    printf("=== M04 EtherCAT Device Test ===\n");
    printf("================================\n\n");

    if (argc >= 2) {
        adapterName = argv[1];// 用户在命令行敲了网卡名，直接拿来用
        printf("Using adapter from command line\n");
    }
    else {
        adapterName = DEFAULT_ADAPTER;// 用户直接双击运行，使用代码里写死的默认网卡
        printf("Using default adapter configuration\n");
    }

    printf("Adapter: %s\n\n", adapterName); //打印网卡名称

	printf("Initializing SOEM...\n");  //初始话SOEM库，打开网卡，准备发报文

    //打开 Npcap 网卡句柄   开始扫描 初筛话
    if (ecx_init(&ctx, adapterName)) {
        printf("SOEM initialization succeeded!\n");

        printf("\nScanning EtherCAT slaves...\n");


        //发第一条真报文 BRD（广播读）数出从站数 → 逐站读 SII EEPROM 身份填进 slavelist[]。返回值 = 从站数，>0 才继续
	   //初始化是从站数量大于0才继续，ecx_config_init()会读取每个从站的 EEPROM，获取其身份信息、PDO 配置等，并填充到 ctx.slavelist[] 中
        if (ecx_config_init(&ctx) > 0) {
			printf("Found %d slave(s)\n", ctx.slavecount);  //打印识别到的从站数量
            //列出从站信息 
            if (ctx.slavecount > 0) {
                for (cnt = 1; cnt <= ctx.slavecount; cnt++) {
					printSlaveInfo(&ctx, cnt); //打印从站信息
                }
				//确定从站数量后，进行 PDO 映射配置和分布式时钟配置
                printf("\nConfiguring PDO mapping...\n");
                
				 // 这里的 IOmap 是主站的进程数据映射区，SOEM 会根据从站的 PDO 配置把数据映射到这个缓冲区
                //过程数据对象（PDO）内存映射与从站 FMMU 硬件配置的核心函数
                //IOmap 保存数据地方   0第0个站 返回值通常是146+136
                //主站通过 SDO 读取从站的 0x1C12（RxPDO 映射  工控机的output ）和 0x1C13（TxPDO 映射）字典；
                ecx_config_map_group(&ctx, IOmap, 0);//0就是全部保存在grpop0

                printf("Configuring distributed clocks...\n");
				//分布式时钟配置，确保主站和从站的时钟同步
                ecx_configdc(&ctx);

                printf("\nWaiting for slaves to enter SAFE-OP state...\n");             
				// 计算期望的工作计数（WKC）值，WKC = 输出字节数 * 2 + 输入字节数
                //M04 输出从站  146 outputsWKC=1 ；inputsWKC = 1。
                //expected= (1 * 2) + 1 = 3  
                expected = (ctx.grouplist[0].outputsWKC * 2) + ctx.grouplist[0].inputsWKC; //group0
				// 进入 SAFE-OP 状态 slavelist[0]命令所有从站   预运行状态
                ////  发起状态跳转请求
                ctx.slavelist[0].state = EC_STATE_SAFE_OP;
				//对于0站，ecx_writestate()会向所有从站发送状态请求，要求它们进入SAFE-OP状态
                ecx_writestate(&ctx, 0);
                chk = 200;
                do {
                    ecx_send_processdata(&ctx);
                    ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
                    ecx_statecheck(&ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
                } while (chk-- && (ctx.slavelist[0].state != EC_STATE_SAFE_OP));
				//SAFE-OP 状态下，主站可以读写从站的 PDO，但从站不会执行任何动作。只有进入 OP 状态后，从站才会开始执行控制逻辑。
                 
                 
                if (ctx.slavelist[0].state == EC_STATE_SAFE_OP) {
                    printf("Slaves entered SAFE-OP state!\n");
                    //发起进入OP状态的请求
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


                        //截至上述 成功进入OP状态

                        printf("\nPDO Size Info:\n");
                        printf("  Output (Master->M04): %d bytes\n", ctx.slavelist[1].Obytes);
                        printf("  Input  (M04->Master): %d bytes\n", ctx.slavelist[1].Ibytes);

                        printf("\n=== Transparent Mode Test (pulsed DataValid) ===\n");
                        printf("Send 'HELLO\\n' every %d ms, DataValid held high %d ms per pulse\n\n",
                               TX_INTERVAL_TICKS * 10, TX_PULSE_TICKS * 10);

                        // ================================================================
                        // PDO 数据窗口布局说明（实测反推，Obytes=146 / Ibytes=136）
                        // ================================================================
                        //
                        // 【输出区 outputs[]：主站 → M04，共146字节】
                        //   [0]      端口使能掩码：bit0=RS485_A  bit1=B  bit2=C  bit3=D
                        //            （这里 0x01 = 只开 RS485_A 口的透传通道）
                        //   [1~9]    保留
                        //   [10]     SLOT-01(RS485_A) DataValid —— 透传发送使能位
                        //   [11]     SLOT-01 DataLength —— 本帧有效字节数
                        //   [12~43]  SLOT-01 Payload —— 要透传出去的原始字节(最大32B)
                        //   [44~77]  SLOT-02 窗口（结构同上：Valid/Len/32B Data）
                        //   [78~111] SLOT-03 窗口（结构同上）
                        //
                        // 【输入区 inputs[]：M04 → 主站，共136字节】
                        //   Input[0]        帧序号 Index（M04 收到新帧时更新）
                        //   Input[1]        本帧接收长度 Len
                        //   Input[2..1+Len] 收到的原始字节（例如 JCOM 发来的
                        //                   A5 5A 0A 01 01 B6 6B → 00 07 A5 5A 0A ...）
                        //   注意：窗口一直保留"最后一帧"，Len 不会自动清零。
                        //   所以接收侧必须自己做新旧帧比较，否则每拍都打印旧帧。
                        // ================================================================

                        // 清零输出区，避免残留旧数据被 M04 当作有效帧发出 向M04的输出
                        memset(ctx.slavelist[1].outputs, 0, ctx.slavelist[1].Obytes);

                        // === 主机 -> M04 发送区说明 ===
                        // 主站向 M04 发送数据的方式是：
                        //  1) 在 ctx.slavelist[1].outputs[] 中写入要透传的数据及控制字（端口使能、Len、Payload、DataValid/触发位等）
                        //     例如：outputs[0]=端口使能, outputs[11]=Len, outputs[12..]=Payload, outputs[10]=DataValid(触发位)
                        //  2) 调用 ecx_send_processdata(&ctx) 将这些 PDO 写入总线上，由 M04 读取并在其 RS-485 口透传出去。
                        //  所以任何写 outputs[] 并调用 ecx_send_processdata 的位置都是“主机发数据到 M04”的实现点。


                        // 接收侧快照：保存上一帧内容，用于"只打印新到的帧"
                        uint8 lastFrame[36];
                        int   lastLen = -1;      // -1 = 还没收到过任何帧
                        memset(lastFrame, 0, sizeof(lastFrame));

                        int totalTx = 0;         // 实际发出的脉冲计数

                        // 主循环：2000拍 × 10ms = 20秒测试窗口 
                        for (int i = 0; i < 2000; i++) {
                            // phase = 本轮(200拍=2秒)内走到第几拍
                            int phase = i % TX_INTERVAL_TICKS;

                            // 1. 端口使能与数据内容常驻（保持完整性，绝不清零长度）
                            ctx.slavelist[1].outputs[0] = 0x01;                   // 使能 RS485_A
                            ctx.slavelist[1].outputs[11] = 6;                     // 数据长度固定为 6 字节
                            memcpy(&ctx.slavelist[1].outputs[12], "HELLO\n", 6); // 载荷常驻

                            // 2. 仅在第 0 拍（每 2 秒）触发一次数值翻转
                            if (phase == 0) {
                                static uint8_t tx_toggle = 0;

                                // 触发核心：每次给一个新数字 (1 -> 2 -> 3 ... 255 -> 0)
                                // M04 检测到 outputs[10] 变动，立刻原子抓取 [11]长度 和 [12..]数据发送
                                ctx.slavelist[1].outputs[10] = ++tx_toggle;

                                totalTx++;
                                printf("[t=%d.%03ds] TX #%d 'HELLO\\n' (Trigger ID=%d)\n",
                                    i / 100, (i % 100) * 10, totalTx, tx_toggle);
                            }


                                          
                            // ---- 3. EtherCAT 周期收发（每拍一帧过程数据）----
                            //实际发送函数
                            ecx_send_processdata(&ctx);// 广播函数
                            ecx_receive_processdata(&ctx, EC_TIMEOUTRET); //抓取回来

                            // ---- 4. 接收处理：与上一帧比较，仅新帧打印 ----
                            // === M04 -> 主站 / 透传来自 RS-485 的数据 ===
                            // M04 把它从 RS-485 接收到的帧放到 inputs[] 窗口中，主站通过 ecx_receive_processdata(&ctx)
                            // 获取过程数据后可以读取 ctx.slavelist[1].inputs：
                            //   inputs[0] = 帧序号, inputs[1] = 长度, inputs[2..] = 数据字节
         
                            uint8* in = ctx.slavelist[1].inputs;
                            uint8  idx = in[0];
                            uint8  len = in[1];

                            if (len > 0 && len <= 32) {
                                // 长度或内容任一变化 → 视为新帧
                                if (lastLen != (int)len || memcmp(lastFrame, &in[2], len) != 0) {
                                    memcpy(lastFrame, &in[2], len);
                                    lastLen = len;
                                    printf("[t=%d.%03ds] RX new frame: idx=%02X len=%d  data: %.*s\n",
                                        i / 100, (i % 100) * 10, idx, len, (int)len, &in[2]);
                                }
                            }

                     

                            osal_usleep(10000);   // 10ms 一拍
                        }
                        printf("\n=== Test completed! %d pulses in 20s ===\n", totalTx);

                    }
                    else {
                        printf("Failed to enter OP state!\n");
                        printf("Current state: 0x%02X\n", ctx.slavelist[0].state);
                    }
                }
                else {
                    printf("Failed to enter SAFE-OP state!\n");
                    printf("Current state: 0x%02X\n", ctx.slavelist[0].state);
                }

                printf("\n=== Final Slave Status ===\n");
                for (cnt = 1; cnt <= ctx.slavecount; cnt++) {
                    printf("Slave %d: State=0x%02X, Lost=%d\n",
                        cnt,
                        ctx.slavelist[cnt].state,
                        ctx.slavelist[cnt].islost);
                }
            }
        }
        else {
            printf("No EtherCAT slaves found!\n");
        }

        printf("\nClosing SOEM...\n");
        ecx_close(&ctx);
        printf("SOEM closed\n");

    }
    else {
        printf("SOEM initialization failed!\n");
    }

    printf("\n=== Test Complete ===\n");
    printf("Press Enter to exit...\n");
    getchar();

    return 0;
}