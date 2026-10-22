#ifndef MINEBEA_CONTROL_H
#define MINEBEA_CONTROL_H

#include "MotorController.h"
#include "driver.h"
#include "M04_ECAT.h"

/* ============================================================
 * MinebeaControl —— 美蓓亚电机应用层(经 M04 网关 Modbus RTU 主站接入)
 *
 * 分层: 本类只做寄存器语义翻译(3000H 命令块/3021H 状态/3009H 清警)
 *       与 mm<->脉冲换算; PDO 窗口字节偏移/字节序/触发时序全在 M04_ECAT。
 *
 * 实例粒度: 1 个 M04 从站 = 1 个 MinebeaControl 实例 = 最多 3 台电机
 *       (Axis_Index_1/2/3 -> 站号拨码 1/2/3 -> SLOT-02/03 / 04/05 / 06/07)
 *       单台调试时 motor_nums 传 1 即可, 代码结构不变。
 *
 * 运动模式: 3000H=7 绝对定位(绝对值编码器, 无回零漂移; 相对指令在下发前
 *           按当前编码器位置换算成绝对目标)   3000H=2 速度控制(点动)
 * 到位判定: 目标位置与 3026H 编码器位置偏差窗口 + 连续稳定周期校验
 *           (3024H 指令位置不在当前读窗 0x3026 起 32 字内, 用下发目标代替;
 *            不依赖 3902H bit1 的模糊到位窗口)
 * 加减速:   0810H/0812H 为驱动器参数, 需一次性设定(写槽未含参数通道),
 *           运动指令只带速度(3002H/3003H)
 * ============================================================ */

class MinebeaControl : public MotorController
{
public:
  MinebeaControl();
  ~MinebeaControl() override = default;

  // MotorController接口实现
  bool init(uint16_t slaveIndex, uint8_t motor_nums, int32_t *zero_pos) override;
  bool setupPDO() override;
  int getMotorNum() const override;
  MotorConfig_t getMotorConfig() const override;

  bool enable(AxisIndex_t motor_index) override;
  bool disable(AxisIndex_t motor_index) override;
  bool reset(AxisIndex_t motor_index) override;

  bool setMotionCommand(AxisIndex_t motor_index, const MotionCommand_t& cmd) override;
  bool setMotionParameter(AxisIndex_t motor_index, const MotionParameter_t& param) override;
  bool setMotionWorkMode(AxisIndex_t motor_index, WorkMode_t w_mode) override;
  bool executeMotion(AxisIndex_t motor_index) override;
  bool stopMotion(AxisIndex_t motor_index) override;
  bool setMotionPosition(AxisIndex_t motor_index, double position, double velocity, bool is_relative) override;

  MotorStatus_t getStatus(AxisIndex_t motor_index) const override;
  void          printControlStatus(AxisIndex_t motor_index) override;
  bool          isTargetReached(AxisIndex_t motor_index) const override;

  bool setParameters(AxisIndex_t motor_index, const std::vector<std::pair<uint16_t, uint32_t>>& params) override;
  bool getParameter(uint16_t index, uint8_t subindex, uint32_t* value) override;

  void processPDO(PdoCtl_t& pdoCtl) override;

  MotorType_t getType() const override
  {
    return MOTOR_TYPE_M04;
  }
  const char* getName() const override
  {
    return "Minebea";
  }

private:
  /* 每轴运行时状态 */
  typedef struct
  {
    MotorControl_t   ctr;          // 工作模式/工作状态/使能标志/对外状态
    MotionCommand_t  current;      // 当前执行命令(位置单位: 脉冲)
    MotionCommand_t  pending;      // 待执行命令
    M04StatusBlock_t status;       // 最近一帧 3021H 状态块
    int32_t  zero_pos;             // 绝对零点(编码器脉冲坐标)
    int16_t  last_alarm;           // 报警号变化检测
    uint8_t  arrival_cnt;          // 到位稳定计数
    int32_t  last_pos;             // 上一拍编码器位置(换目标停稳判定用)
    uint8_t  stable_cnt;           // 换目标停稳判定: 编码器连续不变拍数
    uint8_t  cmd_active;           // 命令块已发到驱动器(写槽 Rising 已完成)
    uint8_t  clear_req;            // 清警请求
    uint8_t  stop_req;             // 停止请求
  } AxisRuntime_t;

  void processAxis(uint8_t axis);
  void processMotionControl(uint8_t axis);
  bool queueCmdBlock(uint8_t axis, uint16_t op, int32_t target, int32_t speed, uint16_t stopOpt);
  bool queueStopBlock(uint8_t axis, uint16_t stopOpt);

  // 运行参数转换: 直线关节 mm <-> 脉冲
  int32_t LengthToPulse(double len) const;
  double  PulseToLength(int32_t pulse) const;

  uint8_t axisOf(AxisIndex_t index) const;   // Axis_Index_All 返回 0xFF

  M04_ECAT        m04_;
  AxisRuntime_t   axes_[M04_AXIS_MAX];
  MotionParameter_t motionParameter_;

  struct
  {
    uint32_t profileVelocity;
    uint32_t profileAcceleration;
    uint32_t profileDeceleration;
  } parameters_;
};

#endif  // MINEBEA_CONTROL_H
