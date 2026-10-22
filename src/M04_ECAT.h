#ifndef M04_ECAT_H
#define M04_ECAT_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * M04_ECAT —— EC2-MB-M04 网关(EtherCAT <-> RS485 Modbus RTU 主站)传输层
 *
 * 职责边界: 只管 PDO 窗口读写、小端字节序、Rising 触发时序。
 * 寄存器语义(3000H 命令块 / 3021H 报警 / 3026H 编码器)由 MinebeaControl 解释。
 *
 * ---- M04 配置工具 SLOT 配置(当前实测布局, Obytes=76 / Ibytes=98) ----
 *  SLOT-01 透传     RS485_A  (接电脑 JCOM, 本驱动不碰它的窗口)
 *  SLOT-02 FC03 读  RS485_B  Slave=1 起始=0x3026 字数=32  Force Poll
 *  SLOT-03 FC16 写  RS485_B  Slave=1 起始=0x3000 字数=9   Rising
 *  (建议补配) SLOT-04 FC06 写 RS485_B Slave=1 起始=0x3009 字数=1 Rising  网络清警
 *
 *  PDO 窗口布局(与 test_m04.cpp 实测一致):
 *  输出区 76B: [0]Modbus Enable [1~8]Control Byte1~8 [9]保留
 *               [10]SLOT-01 DataValid [11]SLOT-01 Len [12~43]SLOT-01 Payload
 *               [44~75]SLOT-03 窗口 Word1~16 (九字命令块占 [44~61])
 *               若加 SLOT-04: 清警字占 [76~77], Obytes=78
 *  输入区 98B: [0]SLOT-01 Index [1]SLOT-01 Len [2~33]SLOT-01 Payload
 *               [34~97]SLOT-02 窗口 Word1~32 (0x3026~0x3045)
 *
 *  注意:
 *  1. 槽 Interface Type: SLOT-01=RS485_A(透传), SLOT-02/03=RS485_B(Modbus 主站,
 *     接美蓓亚驱动器拨码1); 出厂默认 Disable=4, 不改收不到帧
 *  2. 改槽流程: PRE-OP -> 填参数 -> Reload Device -> 写入+保存+关闭网络+启动网络
 *  3. 插删槽位后 PDO 窗口整体重排, 必须在配置工具里核对下方偏移表并修正
 *  4. 读槽用 Force Poll(数据不变也持续刷新), 写槽用 Rising(上升沿发一帧即止)
 *     Rising 时序实测: 控制位先拉低保持2拍(20ms)再抬高, 只发一帧
 *  5. 窗口内一律小端填充, 交换由网关完成; 大端填会导致驱动器回 03 异常拒写
 *  6. 当前读窗起始 0x3026: 3021H报警/3022H转速/3024H指令位置不在窗内,
 *     到位判定改用"目标位置 vs 编码器位置"; 需要报警号时把 SLOT-02 扩为
 *     起始 0x3021 字数 36 并同步修改 M04_STATUS_BLOCK_START/QTY
 * ============================================================ */

#define M04_AXIS_MAX              (3)

/* ---- PDO 窗口字节数(当前实测: Obytes=76 / Ibytes=98) ---- */
/* 加 SLOT-04 清警槽后输出区变为 78 */
#define M04_OUTPUTS_SIZE          (76)
#define M04_INPUTS_SIZE           (98)

#define M04_MODBUS_ENABLE_OFFSET  (0)     /* [0] Modbus Enable */
#define M04_MODBUS_ENABLE_VAL     (0x03)  /* bit0=RS485_A(透传) bit1=RS485_B(Modbus主站) */
#define M04_CTRL_BYTE_BASE        (1)     /* [1..8] Control Byte1~8 */
/* 控制位分配: Byte1 的 bit0~bit7 对应 SLOT-01~08, 依槽序顺延
 * SLOT n -> 偏移 M04_CTRL_BYTE_BASE + (n-1)/8 的 bit((n-1)%8) */

/* ---- 状态读块(FC03, 当前 SLOT-02 配置: 起始 0x3026 字数 32) ---- */
/* 覆盖 0x3026~0x3045:
 *   3026H/3027H 编码器位置   3032H 每转脉冲   3039H/303AH 指令/检出转矩
 * 3021H报警/3022H转速/3024H指令位置不在块内(解析时自动跳过, 返回 0);
 * 需要时把 SLOT-02 扩为起始 0x3021 字数 36, 并同步改下面两个宏 */
#define M04_STATUS_BLOCK_START    (0x3026)
#define M04_STATUS_BLOCK_QTY      (32)

/* ---- 每轴槽位映射: SLOT 号 + 窗口字节偏移 ---- */
typedef struct
{
  uint8_t  readSlot;      /* 状态读槽(1-based SLOT 号) */
  uint16_t readOffset;    /* 该槽输入窗口字节偏移 */
  uint8_t  writeSlot;     /* 运动写槽(九字块 FC16) */
  uint16_t writeOffset;   /* 该槽输出窗口字节偏移 */
  uint8_t  clearSlot;     /* 清警写槽(3009H=1) */
  uint16_t clearOffset;   /* 该槽输出窗口字节偏移 */
} M04SlotMap_t;

/* ---- 状态块解析结果 ---- */
typedef struct
{
  int16_t alarmNo;        /* 3021H 当前报警号, 0=无报警 */
  int16_t speedRpm;       /* 3022H 当前转速 rpm */
  int32_t cmdPos;         /* 3024H 指令位置 count */
  int32_t encoderPos;     /* 3026H 编码器位置 count(绝对值, 掉电保持) */
  int16_t pulsePerRev;    /* 3032H 每转脉冲, 出厂 16000 */
  int16_t torqueCmd;      /* 3039H 指令转矩 */
  int16_t torqueActual;   /* 303AH 检出转矩 */
} M04StatusBlock_t;

class M04_ECAT
{
public:
  M04_ECAT();

  /* 绑定 EtherCAT 周期缓冲区(输出区=主站->网关, 输入区=网关->主站) */
  bool bind(uint8_t* outputs, uint8_t* inputs, size_t outSize, size_t inSize);

  /* 每个 EtherCAT 周期调用一次: 维护 Modbus Enable, 推进触发时序 */
  void processCycle();

  /* 排队九字块(3000H~3008H): 填写写窗口并走"拉低->保持->抬高"触发时序。
   * 上一条未发完时返回 false */
  bool writeCmdBlock(uint8_t axis, const uint16_t reg9[9]);

  /* 排队网络清警(3009H=1), 走清警槽 */
  bool requestClearAlarm(uint8_t axis);

  bool isWriteBusy(uint8_t axis) const;
  bool takeWriteDone(uint8_t axis);    /* 读清"九字块已发完"事件 */
  bool takeClearDone(uint8_t axis);    /* 读清"清警帧已发完"事件 */

  /* 解析该轴状态读窗口(小端字节序 -> 寄存器值) */
  bool readStatusBlock(uint8_t axis, M04StatusBlock_t& out) const;

  /* 字节序工具: 窗口内每个 16 位字小端存放; 32 位寄存器高字在前 */
  static uint16_t rdWord(const uint8_t* p);
  static void     wrWord(uint8_t* p, uint16_t v);
  static int32_t  rdDword(const uint8_t* p);
  static void     wrDword(uint8_t* p, int32_t v);

private:
  /* 触发时序: stage0 空闲 -> stage1 数据就位+触发位拉低(保持若干拍) -> stage2 抬高(发一帧) */
  typedef struct
  {
    uint8_t  slot;
    uint16_t dataOffset;
    uint16_t dataLen;
    uint8_t  stage;
    uint8_t  lowCycles;
    bool     done;
  } Trigger_t;

  void setTriggerBit(uint8_t slot, bool high);
  bool arm(Trigger_t& trig, const uint8_t* data, uint16_t len);

  uint8_t* outputs_;
  uint8_t* inputs_;
  bool     bound_;

  Trigger_t cmdTrig_[M04_AXIS_MAX];    /* 九字块写槽 */
  Trigger_t clearTrig_[M04_AXIS_MAX];  /* 清警写槽 */
};

#endif  /* M04_ECAT_H */
