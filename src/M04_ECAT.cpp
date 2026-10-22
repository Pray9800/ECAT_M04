#include "M04_ECAT.h"

#include <string.h>

/* ============================================================
 * M04 SLOT 配置和 PDO 窗口布局
 *
 * | SLOT    | 模块              | 功能         | 485口   | 站号     | 数据            | 模式       | 输出窗          | 输入窗         |
 * |---------|-------------------|--------------|---------|----------|-----------------|------------|-----------------|----------------|
 * | SLOT-01 | 0x0900 透传(2304) | HELLO 收发   | RS485_A | —        | 32B 载荷        | Force Poll | [9~43] 35B      | [0~33] 34B     |
 * | SLOT-02 | FC03 读           | 电机1 状态读 | RS485_B | 1(拨码1) | 0x3026 起 32 字 | Force Poll | —               | [34~97] 64B    |
 * | SLOT-03 | FC16 写           | 电机1 命令写 | RS485_B | 1        | 0x3000 起 9 字  | Rising     | [44~171] 128B   | —              |
 * | SLOT-04 | FC03 读           | 电机2 状态读 | RS485_C | 2(拨码2) | 0x3026 起 32 字 | Force Poll | —               | [98~161] 64B   |
 * | SLOT-05 | FC16 写           | 电机2 命令写 | RS485_C | 2        | 0x3000 起 9 字  | Rising     | [172~299] 128B  | —              |
 * | SLOT-06 | 占位              | 未配         | —       | —        | —               | —          | —               | [162~225] 64B   |
 * | SLOT-07 | 占位              | 未配         | —       | —        | —               | —          | [300~427] 128B  | —              |
 *
 * 输入 PDO 区：
 *   [0~33]     SLOT-01 透传，34 字节
 *   [34~97]    SLOT-02 电机1状态，64 字节
 *   [98~161]   SLOT-04 电机2状态，64 字节
 *   [162~225]  SLOT-06 占位，64 字节
 *
 * 输出 PDO 区：
 *   [0]         Modbus Enable
 *   [1~8]       Control Byte
 *   [9~43]      SLOT-01 透传，35 字节
 *   [44~171]    SLOT-03 电机1命令，128 字节
 *   [172~299]   SLOT-05 电机2命令，128 字节
 *   [300~427]   SLOT-07 占位，128 字节
 *
 * 窗口大小：Obytes=428，Ibytes=226。
 * clearSlot/clearOffset 为清警槽及其输出偏移；当前均为 0，表示未配置清警槽。
 * 插删 SLOT 后窗口会整体重排，必须同步核对本表和 kSlotMap。
 * ============================================================ */
static const M04SlotMap_t kSlotMap[M04_AXIS_MAX] = {
  /* readSlot, readOffset, writeSlot, writeOffset, clearSlot, clearOffset */
    //对应axis = 0
  { 2,  34, 3, 44,  0,  0 },   /*  偏移量 轴0: 拨码1, SLOT-02读/SLOT-03写  0-33被SLOT1 使用 所有34 */ 
  { 4,  98, 5, 172,  0,  0 },   /* 轴1: 拨码2, SLOT-04读/SLOT-05写 */
  { 6,  162, 7,  300,  0,  0 },   /* 轴2: 未配置 */
};

#define M04_CMD_BLOCK_BYTES   (18)   /* 3000H~3008H 共 9 字 */
#define M04_CLEAR_BYTES       (2)    /* 3009H 单字 */
#define M04_STATUS_BYTES      (M04_STATUS_BLOCK_QTY * 2)   /* 32 字 = 64B */
#define M04_TRIGGER_LOW_CYCLES (2)   /* 触发位拉低保持拍数(50ms/拍, 覆盖网关约15ms内部轮询) */

M04_ECAT::M04_ECAT()
    : outputs_(nullptr), inputs_(nullptr), bound_(false)
{
  memset(cmdTrig_, 0, sizeof(cmdTrig_));
  memset(clearTrig_, 0, sizeof(clearTrig_));
}

bool M04_ECAT::bind(uint8_t* outputs, uint8_t* inputs, size_t outSize, size_t inSize)
{
  if (!outputs || !inputs)
    return false;

  /* 按偏移表推算窗口最小需求, 实际窗口不小于该值即可 */
  size_t outNeed = M04_CTRL_BYTE_BASE + 8;
  size_t inNeed  = 0;
  for (int i = 0; i < M04_AXIS_MAX; i++)
  {
    if (kSlotMap[i].writeSlot == 0 && kSlotMap[i].readSlot == 0)
      continue;   /* 未配置的轴不占窗口 */
    size_t wEnd = (size_t)kSlotMap[i].writeOffset + M04_CMD_BLOCK_BYTES;
    size_t cEnd = (size_t)kSlotMap[i].clearOffset + M04_CLEAR_BYTES;
    size_t rEnd = (size_t)kSlotMap[i].readOffset + M04_STATUS_BYTES;
    if (kSlotMap[i].writeSlot != 0 && wEnd > outNeed) outNeed = wEnd;
    if (kSlotMap[i].clearSlot != 0 && cEnd > outNeed) outNeed = cEnd;
    if (kSlotMap[i].readSlot  != 0 && rEnd > inNeed)  inNeed  = rEnd;
  }

  if (outSize < outNeed || inSize < inNeed)
    return false;

  outputs_ = outputs;
  inputs_  = inputs;

  if (!bound_)
  {
    for (int i = 0; i < M04_AXIS_MAX; i++)
    {
      cmdTrig_[i].slot       = kSlotMap[i].writeSlot;
      cmdTrig_[i].dataOffset = kSlotMap[i].writeOffset;
      cmdTrig_[i].dataLen    = M04_CMD_BLOCK_BYTES;

      clearTrig_[i].slot       = kSlotMap[i].clearSlot;
      clearTrig_[i].dataOffset = kSlotMap[i].clearOffset;
      clearTrig_[i].dataLen    = M04_CLEAR_BYTES;
    }
    bound_ = true;
  }

  return true;
}

void M04_ECAT::processCycle()
{
  if (!bound_)
    return;

  outputs_[M04_MODBUS_ENABLE_OFFSET] = M04_MODBUS_ENABLE_VAL; //保存M04 Modbus Enable=1, 让网关透传 Modbus 帧

  Trigger_t* groups[2] = { cmdTrig_, clearTrig_ };
  for (int g = 0; g < 2; g++)
  {
    for (int i = 0; i < M04_AXIS_MAX; i++)
    {
      Trigger_t& trig = groups[g][i];

      if (trig.stage == 1)      /* 数据就位, 触发位已拉低, 保持若干拍 */
      {
        trig.lowCycles++;
        if (trig.lowCycles >= M04_TRIGGER_LOW_CYCLES)
        {
          setTriggerBit(trig.slot, true);   /* 0->1 上升沿, 网关发一帧 触发信号 */ 
          trig.stage = 2;
        }
      }
      else if (trig.stage == 2) /* 发完挂起事件, 触发位保持高, 下次 arm 先拉低再造沿 */
      {
        trig.done  = true;
        trig.stage = 0;
      }
    }
  }
}

bool M04_ECAT::writeCmdBlock(uint8_t axis, const uint16_t reg9[9])  //命令写进去
{
  if (axis >= M04_AXIS_MAX || !reg9)
    return false;

  uint8_t buf[M04_CMD_BLOCK_BYTES];
  for (int i = 0; i < 9; i++)
    wrWord(buf + i * 2, reg9[i]);

  return arm(cmdTrig_[axis], buf, M04_CMD_BLOCK_BYTES);
}

bool M04_ECAT::requestClearAlarm(uint8_t axis)
{
  if (axis >= M04_AXIS_MAX)
    return false;

  uint8_t buf[M04_CLEAR_BYTES];
  wrWord(buf, 0x0001);          /* 3009H = 1: Alarm Clear */

  return arm(clearTrig_[axis], buf, M04_CLEAR_BYTES);
}

bool M04_ECAT::isWriteBusy(uint8_t axis) const
{
  if (axis >= M04_AXIS_MAX)
    return false;
  return cmdTrig_[axis].stage != 0;
}

bool M04_ECAT::takeWriteDone(uint8_t axis)
{
  if (axis >= M04_AXIS_MAX || !cmdTrig_[axis].done)
    return false;
  cmdTrig_[axis].done = false;
  return true;
}

bool M04_ECAT::takeClearDone(uint8_t axis)
{
  if (axis >= M04_AXIS_MAX || !clearTrig_[axis].done)
    return false;
  clearTrig_[axis].done = false;
  return true;
}

bool M04_ECAT::readStatusBlock(uint8_t axis, M04StatusBlock_t& out) const
{
  if (!bound_ || axis >= M04_AXIS_MAX || kSlotMap[axis].readSlot == 0)
    return false;

  const uint8_t* p = inputs_ + kSlotMap[axis].readOffset;

  /* 寄存器 -> 窗口字偏移 = 寄存器 - 块起始(当前 0x3026);
   * 不在读窗内的寄存器(如 3021H/3022H/3024H)按默认值 0 返回 */
  auto rdWordAt = [p](uint16_t reg, int32_t def) -> int32_t {
    int idx = (int)reg - (int)M04_STATUS_BLOCK_START;
    if (idx < 0 || idx + 1 > M04_STATUS_BLOCK_QTY) return def;
    return (int32_t)rdWord(p + 2 * idx);
  };
  auto rdDwordAt = [p](uint16_t reg, int32_t def) -> int32_t {
    int idx = (int)reg - (int)M04_STATUS_BLOCK_START;
    if (idx < 0 || idx + 2 > M04_STATUS_BLOCK_QTY) return def;
    return rdDword(p + 2 * idx);
  };

  out.alarmNo      = (int16_t)rdWordAt(0x3021, 0);   /* 3021H 报警号(当前窗外) */
  out.speedRpm     = (int16_t)rdWordAt(0x3022, 0);   /* 3022H 转速(当前窗外) */
  out.cmdPos       = rdDwordAt(0x3024, 0);           /* 3024H/3025H 指令位置(当前窗外) */
  out.encoderPos   = rdDwordAt(0x3026, 0);           /* 3026H/3027H 编码器位置 */
  out.pulsePerRev  = (int16_t)rdWordAt(0x3032, 0);   /* 3032H 每转脉冲 */
  out.torqueCmd    = (int16_t)rdWordAt(0x3039, 0);   /* 3039H 指令转矩 */
  out.torqueActual = (int16_t)rdWordAt(0x303A, 0);   /* 303AH 检出转矩 */

  return true;
}

void M04_ECAT::setTriggerBit(uint8_t slot, bool high)
{
  if (!bound_ || slot < 1)
    return;

  uint16_t byteIdx = M04_CTRL_BYTE_BASE + (uint16_t)((slot - 1) / 8);
  uint8_t  mask    = (uint8_t)(1u << ((slot - 1) % 8));

  if (high)
    outputs_[byteIdx] |= mask;
  else
    outputs_[byteIdx] &= (uint8_t)~mask;
}

bool M04_ECAT::arm(Trigger_t& trig, const uint8_t* data, uint16_t len)
{
  if (!bound_ || trig.slot == 0 || trig.stage != 0 || len > trig.dataLen)
    return false;   /* slot==0: 该功能槽未在 M04 配置(如清警槽) */

  if (data && len > 0)
    memcpy(outputs_ + trig.dataOffset, data, len);

  setTriggerBit(trig.slot, false);  /* 先拉低, 保持后抬高造上升沿 */
  trig.stage     = 1;
  trig.lowCycles = 0;
  trig.done      = false;

  return true;
}

/* ---- 字节序工具: 窗口内每个 16 位字小端存放(交换由网关完成),
 *      32 位寄存器高字在前(3002H=速度高/3003H=速度低, 3024H=位置高/3025H=位置低) ---- */
uint16_t M04_ECAT::rdWord(const uint8_t* p)
{
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

void M04_ECAT::wrWord(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

int32_t M04_ECAT::rdDword(const uint8_t* p)
{
  return (int32_t)(((uint32_t)rdWord(p) << 16) | rdWord(p + 2));
}

void M04_ECAT::wrDword(uint8_t* p, int32_t v)
{
  wrWord(p, (uint16_t)(((uint32_t)v) >> 16));
  wrWord(p + 2, (uint16_t)((uint32_t)v));
}
