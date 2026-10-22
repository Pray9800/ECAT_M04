//C语言基本的两个库
#include <stdio.h>
#include <string.h>

// 美蓓亚驱动层(先引 C++ 标准库, 避免 pcap 头宏定义 inline 与标准库冲突;
// soem 经 driver.h 以 extern "C" 引入, ecx_* 保持 C 链接)
#include "MinebeaControl.h"

#define EC_TIMEOUTMON 500  //检查网线有没有掉 暂时没用到

// ============================================================
// 主循环节奏: 每拍 = 10ms (M04 触发时序按 10ms/拍计)
// ============================================================
#define CYCLE_TICKS_PER_SEC 100

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

// ============================================================
// 美蓓亚驱动层调试序列(单台电机, 拨码站号1 -> SLOT-02读/SLOT-03写)
//
// 流程: 伺服开 -> 绝对定位 +5mm -> 到位后绝对定位回零位 -> 伺服关
//
// PDO 窗口布局(由 M04_ECAT 按当前 SLOT 配置解析):
//   输出区 76B: [0]Modbus Enable [1~8]Control Byte
//                [10~43]SLOT-01 透传窗口(本测试不碰) [44~75]SLOT-03 九字写窗
//   输入区 98B: [0~33]SLOT-01 透传窗口 [34~97]SLOT-02 状态读窗(0x3026 起 32 字)
// ============================================================
typedef enum {
    SEQ_ENABLE,      // 发使能(内部自动走伺服开 3000H=0/3004H=3)
    SEQ_WAIT_SERVO,  // 等伺服开写槽 Rising 发完
    SEQ_MOVE_POS,    // 下发 +5mm 绝对定位
    SEQ_WAIT_POS,    // 等到位
    SEQ_MOVE_ZERO,   // 下发 0mm 绝对定位(回零位)
    SEQ_WAIT_ZERO,   // 等到位
    SEQ_FINISH       // 伺服关, 收尾
} SeqStep_t;

static const char* seqStepName(SeqStep_t s) {
    switch (s) {
    case SEQ_ENABLE:     return "ENABLE";
    case SEQ_WAIT_SERVO: return "WAIT_SERVO";
    case SEQ_MOVE_POS:   return "MOVE_POS(+5mm)";
    case SEQ_WAIT_POS:   return "WAIT_POS";
    case SEQ_MOVE_ZERO:  return "MOVE_ZERO(0mm)";
    case SEQ_WAIT_ZERO:  return "WAIT_ZERO";
    case SEQ_FINISH:     return "FINISH";
    }
    return "?";
}

static void runMinebeaDebug(ecx_contextt* context) {
    MinebeaControl ctrl;
    int32_t zero_pos[1] = {0};          // 绝对零点 = 当前坐标 0(绝对值编码器, 无需回零)

    ctrl.init(SLAVE_INDEX_1, 1, zero_pos);   // 单台调试 motor_nums=1, 接三台改 3 即可
    ctrl.setupPDO();

    // 绑定 M04 从站(slave 1)的周期缓冲区
    PdoCtl_t pdo;
    memset(&pdo, 0, sizeof(pdo));
    pdo.cycle_ns    = 10 * 1000 * 1000;     // 10ms
    pdo.output      = context->slavelist[1].outputs;
    pdo.input       = context->slavelist[1].inputs;
    pdo.output_size = context->slavelist[1].Obytes;
    pdo.input_size  = context->slavelist[1].Ibytes;

    printf("\n=== Minebea Debug Sequence (single motor, slave=1) ===\n");
    printf("PDO window: out=%d bytes, in=%d bytes\n",
           context->slavelist[1].Obytes, context->slavelist[1].Ibytes);
    printf("Sequence: Servo-on -> +5mm abs -> 0mm abs -> Servo-off\n\n");

    SeqStep_t step = SEQ_ENABLE;
    int tick = 0;
    int step_tick = 0;
    int flush_tick = 0;

    while (1) {
        // ---- 1. 驱动层周期处理: 绑定窗口 + Modbus Enable 保持 + 触发时序 + 状态解析 ----
        ctrl.processPDO(pdo);

        // ---- 2. EtherCAT 周期收发(主机 <-> M04 的过程数据交换点) ----
        ecx_send_processdata(context);
        ecx_receive_processdata(context, EC_TIMEOUTRET);

        // ---- 3. 调试序列推进 ----
        switch (step) {
        case SEQ_ENABLE:
            ctrl.enable(Axis_Index_1);
            step = SEQ_WAIT_SERVO;
            step_tick = 0;
            printf("[seq] -> %s\n", seqStepName(step));
            break;

        case SEQ_WAIT_SERVO:
            if (step_tick >= 50) {   // 伺服开写槽 Rising 走完约 3 拍, 留 500ms 余量
                ctrl.setMotionPosition(Axis_Index_1, 5.0, 2.0, false);   // +5mm, 2mm/s, 绝对
                step = SEQ_WAIT_POS;
                step_tick = 0;
                printf("[seq] -> %s\n", seqStepName(step));
            }
            break;

        case SEQ_MOVE_POS:   // 与 SEQ_WAIT_POS 合并处理, 保留枚举语义
        case SEQ_WAIT_POS:
            if (ctrl.isTargetReached(Axis_Index_1)) {
                ctrl.setMotionPosition(Axis_Index_1, 0.0, 2.0, false);   // 回零位, 绝对
                step = SEQ_WAIT_ZERO;
                step_tick = 0;
                printf("[seq] -> %s\n", seqStepName(step));
            }
            else if (step_tick > 60 * CYCLE_TICKS_PER_SEC) {   // 60s 超时保护
                printf("[seq] ERROR: +5mm move timeout, abort\n");
                step = SEQ_FINISH;
                step_tick = 0;
            }
            break;

        case SEQ_MOVE_ZERO:  // 与 SEQ_WAIT_ZERO 合并处理
        case SEQ_WAIT_ZERO:
            if (ctrl.isTargetReached(Axis_Index_1)) {
                printf("[seq] +5mm / 0mm round trip finished\n");
                step = SEQ_FINISH;
                step_tick = 0;
            }
            else if (step_tick > 60 * CYCLE_TICKS_PER_SEC) {
                printf("[seq] ERROR: 0mm move timeout, abort\n");
                step = SEQ_FINISH;
                step_tick = 0;
            }
            break;

        case SEQ_FINISH:
            if (step_tick == 0) {
                ctrl.printControlStatus(Axis_Index_1);
                ctrl.disable(Axis_Index_1);   // 伺服关(3000H=0/3004H=2), 需几拍发出
            }
            if (step_tick > 50) {
                flush_tick = 1;
            }
            break;
        }
        step_tick++;

        // ---- 4. 每秒打印一次状态(报警/编码器/偏差/转速) ----
        if (tick % CYCLE_TICKS_PER_SEC == 0) {
            printf("[t=%d.%02ds] ", tick / CYCLE_TICKS_PER_SEC, (tick % CYCLE_TICKS_PER_SEC) / 10);
            ctrl.printControlStatus(Axis_Index_1);
        }
        tick++;

        if (flush_tick) {
            printf("\n=== Debug sequence done ===\n");
            break;
        }

        osal_usleep(10000);   // 10ms 一拍
    }
}

int main(int argc, char* argv[]) {
    int cnt;
    int expected;
    int chk;
    const char* adapterName;

    printf("=== M04 Minebea Motor Test ===\n");
    printf("==============================\n\n");

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
                //IOmap 保存数据地方   0第0个站
                //主站通过 SDO 读取从站的 0x1C12（RxPDO 映射  工控机的output ）和 0x1C13（TxPDO 映射）字典；
                ecx_config_map_group(&ctx, IOmap, 0);//0就是全部保存在grpop0

                printf("Configuring distributed clocks...\n");
				//分布式时钟配置，确保主站和从站的时钟同步
                ecx_configdc(&ctx);

                printf("\nWaiting for slaves to enter SAFE-OP state...\n");
				// 计算期望的工作计数（WKC）值，WKC = 输出字节数 * 2 + 输入字节数
                expected = (ctx.grouplist[0].outputsWKC * 2) + ctx.grouplist[0].inputsWKC; //group0
				// 进入 SAFE-OP 状态 slavelist[0]命令所有从站   预运行状态
				//  发起状态跳转请求
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

                        // 清零输出区，避免残留旧数据被 M04 当作有效帧发出
                        memset(ctx.slavelist[1].outputs, 0, ctx.slavelist[1].Obytes);

                        // === 主机 -> M04 发送点 ===
                        // M04_ECAT 层在 ctx.slavelist[1].outputs[] 里填 Modbus Enable/触发位/
                        // 九字命令块, 再由下方 ecx_send_processdata 发出;
                        // === M04 -> 主站 接收点 ===
                        // ecx_receive_processdata 后 ctx.slavelist[1].inputs[] 更新,
                        // M04_ECAT 层从状态读窗解析 0x3026 编码器位置等。
                        runMinebeaDebug(&ctx);
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
