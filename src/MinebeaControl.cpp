#include "MinebeaControl.h"

#include <cstring>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <iomanip>

#include "logc.h"

/* ============================================================
 * 函数地图(按阅读顺序, 三类就够)
 *
 * [周期链路] 每 10ms 一拍, 上位机主循环依次调用:
 *   processPDO()            总入口: 绑窗口/Modbus Enable 保持/逐轴处理
 *     processAxis()         每轴: 解 3021H 状态块进缓存 + 报警检测 + 命令完成确认
 *       processMotionControl() 运动状态机: 择机把队列命令交给传输层下发
 *
 * [命令出口] 下单只置标志, 实际发指令由上面周期链路完成:
 *   setMotionPosition()     运动总入口(mm->脉冲换算后排队, 调试改行程/速度改这里)
 *     queueCmdBlock()       拼 3000H~3008H 九字命令块
 *     queueStopBlock()      拼停止块(3000H=0 + 3004H 停止选项)
 *   stopMotion()            减速停, 无条件清空队列
 *
 * [状态读取] 只读本地缓存, 不发总线请求:
 *   getStatus() / isTargetReached() / printControlStatus()
 *
 * 其余为上位机接口样板(参数检查+置标志, 不用细读):
 *   enable/disable/reset/setMotionCommand/setMotionParameter/
 *   setMotionWorkMode/executeMotion/setParameters/getParameter/
 *   init/setupPDO/getMotorNum/getMotorConfig
 * 换算小工具: LengthToPulse/PulseToLength/axisOf
 * ============================================================ */

// ---- 机械参数(直线关节: 丝杠直驱, 无减速) ----
#define MINEBEA_PULSE_PER_REV   (16000)   // 电机一圈脉冲数(3032H 出厂 16000, 上电后可核对)
#define MINEBEA_SCREW_LEAD      (1.5)     // 丝杠导程 mm/圈
#define MINEBEA_RATIO           (1.0)     // 减速比
#define MINEBEA_MOTOR_DIR       (1.0)     // 电机方向, 反向安装改为 -1.0

// ---- 到位判定: 指令位置与编码器位置偏差窗口 + 连续稳定周期 ----
#define MINEBEA_ARRIVAL_WINDOW  (100)     // 偏差窗口 脉冲(10667脉冲/mm, 100脉冲 ≈ 9.4um)
#define MINEBEA_ARRIVAL_STABLE  (5)       // 连续稳定周期数(50ms * 5 = 250ms)

// ---- 默认速度 ----
#define MINEBEA_DEFAULT_VELOCITY_PPS (16000)  // 1 圈/s = 1.5mm/s
#define MINEBEA_INIT_VELOCITY_PPS    (16000)  // 回零位速度
#define MINEBEA_DEMON_DISTANCE_MM    (10.0)   // 演示模式往复行程

// ---- 美蓓亚操作码(3000H) ----
#define MINEBEA_OP_STOP        (0)   // 停止
#define MINEBEA_OP_SPEED       (2)   // 速度控制(速度带符号表方向)
#define MINEBEA_OP_POS_REL     (6)   // 相对定位(编码器基准)
#define MINEBEA_OP_POS_ABS     (7)   // 绝对定位

// ---- 停止选项(3004H, 仅 3000H=0 时生效) ----
#define MINEBEA_STOP_DECEL     (0)   // 减速停
#define MINEBEA_STOP_SERVO_OFF (2)   // 伺服关(自由停车)
#define MINEBEA_STOP_SERVO_ON  (3)   // 伺服开


MinebeaControl::MinebeaControl() //前面是函数名 后面是类名
{
  memset(axes_, 0, sizeof(axes_));
  memset(&motionParameter_, 0, sizeof(motionParameter_));
  memset(&parameters_, 0, sizeof(parameters_));

  parameters_.profileVelocity     = MINEBEA_DEFAULT_VELOCITY_PPS;
  parameters_.profileAcceleration = 0;   // 0810H 加速度: 驱动器参数, 一次性设定
  parameters_.profileDeceleration = 0;   // 0812H 减速度: 同上

  memset(&motorParams_, 0, sizeof(motorParams_)); //电机参数清零
  motorParams_.encoderResolution = MINEBEA_PULSE_PER_REV; //一圈一千六百转
  motorParams_.gearRatio         = MINEBEA_RATIO; //减速比1：1
  motorParams_.dir               = MINEBEA_MOTOR_DIR; //正方向
}

bool MinebeaControl::init(uint16_t slaveIndex, uint8_t motor_nums, int32_t *zero_pos)
{
  slaveIndex_ = (SlaveIndex_t)slaveIndex;
  motor_nums_ = (motor_nums > M04_AXIS_MAX) ? M04_AXIS_MAX : motor_nums;

  for (uint8_t i = 0; i < M04_AXIS_MAX; i++)
  {
    memset(&axes_[i], 0, sizeof(axes_[i]));
    axes_[i].zero_pos     = (zero_pos != nullptr && i < motor_nums_) ? zero_pos[i] : 0;
    axes_[i].ctr.work_state = MOTOR_STATE_RESET;
    axes_[i].ctr.work_mode  = WORK_MODE_MOTOR;
    axes_[i].last_alarm   = -1;
  }

  paramsInitialized_ = true;


  //核心打印 初始化打印  ECAT站号：电机站号  
  for (uint8_t i = 0; i < motor_nums_; i++)
  {
    log_info("Minebea[%d:%d]: init zero position absolute = %d (pulse)",
             slaveIndex_, i, axes_[i].zero_pos);
  }

  return true;
}

bool MinebeaControl::setupPDO()
{
  log_debug("Minebea[%d]: Setting up PDO (slot 偏移表见 M04_ECAT.cpp)", slaveIndex_);
  return true;
}

int MinebeaControl::getMotorNum() const
{
  return motor_nums_;
}

MotorConfig_t MinebeaControl::getMotorConfig() const
{
  return motorParams_;
}

bool MinebeaControl::enable(AxisIndex_t motor_index)
{
  uint8_t axis = axisOf(motor_index);
  if (axis == 0xFF)
  {
    bool re = true;
    for (uint8_t i = 0; i < motor_nums_; i++)
      re = enable((AxisIndex_t)i) && re;
    return re;
  }
  if (axis >= motor_nums_)
    return false;

  log_info("Minebea[%d:%d]: Enabling motor (Servo-on)", slaveIndex_, axis);
  axes_[axis].ctr.flag_enable = 1;
  axes_[axis].ctr.work_state  = MOTOR_STATE_RESET;

  return true;
}

bool MinebeaControl::disable(AxisIndex_t motor_index)
{
  uint8_t axis = axisOf(motor_index);
  if (axis == 0xFF)
  {
    bool re = true;
    for (uint8_t i = 0; i < motor_nums_; i++)
      re = disable((AxisIndex_t)i) && re;
    return re;
  }
  if (axis >= motor_nums_)
    return false;

  log_info("Minebea[%d:%d]: Disabling motor (Servo-off)", slaveIndex_, axis);
  axes_[axis].ctr.flag_enable = 0;   // processAxis 发 3000H=0/3004H=2 后置 DISABLED

  return true;
}

bool MinebeaControl::reset(AxisIndex_t motor_index)
{
  uint8_t axis = axisOf(motor_index);
  if (axis == 0xFF)
  {
    bool re = true;
    for (uint8_t i = 0; i < motor_nums_; i++)
      re = reset((AxisIndex_t)i) && re;
    return re;
  }
  if (axis >= motor_nums_)
    return false;

  log_info("Minebea[%d:%d]: Resetting motor (3009H Alarm Clear)", slaveIndex_, axis);
  axes_[axis].clear_req = 1;

  return true;
}

bool MinebeaControl::setMotionCommand(AxisIndex_t motor_index, const MotionCommand_t& cmd)
{
  uint8_t axis = axisOf(motor_index);
  if (axis >= motor_nums_)
    return false;

  if (cmd.mode != OPERATION_MODE_POSITION && cmd.mode != OPERATION_MODE_VELOCITY)
  {
    log_error("Minebea[%d:%d]: Unsupported operation mode %d", slaveIndex_, axis, cmd.mode);
    return false;
  }

  axes_[axis].pending = cmd; //核心代码 保存脉冲和速度等参数到 pending 中，等待 executeMotion 执行
  log_debug("Minebea[%d:%d]: Motion command set - mode=%d, pos=%d, vel=%u, relative=%d",
            slaveIndex_, axis, cmd.mode, cmd.target_position, cmd.profile_velocity, cmd.flag_relative);

  return true;
}

bool MinebeaControl::setMotionParameter(AxisIndex_t motor_index, const MotionParameter_t &param)
{
  uint8_t axis = axisOf(motor_index);
  if (axis >= motor_nums_)
    return false;

  motionParameter_ = param;
  parameters_.profileVelocity     = param.profile_velocity;
  parameters_.profileAcceleration = param.profile_acceleration;
  parameters_.profileDeceleration = param.profile_deceleration;

  // 0810H/0812H 为驱动器参数寄存器, 写槽未含参数通道, 需一次性设定(手动或扩展写槽)
  log_info("Minebea[%d:%d]: cache motion param vel=%u acc=%u dec=%u (加减速实际取驱动器 0810H/0812H)",
           slaveIndex_, axis, param.profile_velocity, param.profile_acceleration, param.profile_deceleration);

  return true;
}

bool MinebeaControl::setMotionWorkMode(AxisIndex_t motor_index, WorkMode_t w_mode)
{
  uint8_t axis = axisOf(motor_index);
  if (axis >= motor_nums_)
    return false;

  AxisRuntime_t* rt  = &axes_[axis];
  MotorControl_t* ctr = &rt->ctr;

  int32_t  tar_pos = 0;
  uint32_t pro_vel = 0;

  if (w_mode == WORK_MODE_INIT)
  {
    // 绝对值编码器无需回零序列: 直接绝对定位到零位
    ctr->work_mode = WORK_MODE_INIT;

    MotionCommand_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.mode            = OPERATION_MODE_POSITION;
    cmd.target_position = rt->zero_pos;
    cmd.target_velocity = MINEBEA_INIT_VELOCITY_PPS;
    cmd.profile_velocity = MINEBEA_INIT_VELOCITY_PPS;
    cmd.flag_relative   = false;
    cmd.flag_new_target = 1;

    tar_pos = rt->zero_pos;
    pro_vel = MINEBEA_INIT_VELOCITY_PPS;

    setMotionCommand(motor_index, cmd);
  }
  else if (w_mode == WORK_MODE_SET_ZERO)
  {
    // 当前编码器位置设为零位(编码器掉电保持, 需同步更新 motor_zero.txt)
    rt->zero_pos = rt->status.encoderPos;
    ctr->state.flag_offset_init = 1;
    log_info("Minebea[%d:%d]: zero position set, encoder=%d (请更新 motor_zero.txt)",
             slaveIndex_, axis, rt->zero_pos);
  }
  else if (w_mode == WORK_MODE_DEMON)
  {
    ctr->work_mode = WORK_MODE_DEMON;

    MotionCommand_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.mode            = OPERATION_MODE_POSITION;
    cmd.target_position = rt->zero_pos + (int32_t)(MINEBEA_MOTOR_DIR * LengthToPulse(MINEBEA_DEMON_DISTANCE_MM));
    cmd.target_velocity = MINEBEA_DEFAULT_VELOCITY_PPS;
    cmd.profile_velocity = MINEBEA_DEFAULT_VELOCITY_PPS;
    cmd.flag_relative   = false;
    cmd.flag_new_target = 1;

    tar_pos = cmd.target_position;
    pro_vel = MINEBEA_DEFAULT_VELOCITY_PPS;

    setMotionCommand(motor_index, cmd);
  }
  else if (w_mode == WORK_MODE_MOTOR_STOP)
  {
    ctr->work_mode = WORK_MODE_MOTOR;
    rt->stop_req   = 1;
  }
  else if (w_mode == WORK_MODE_MOTOR)
  {
    ctr->work_mode = WORK_MODE_MOTOR;
  }

  log_info("Minebea[%d:%d]: SetWorkMode=%d, target=%d(pulse), vel=%u(pps)",
           slaveIndex_, axis, w_mode, tar_pos, pro_vel);

  return true;
}

bool MinebeaControl::executeMotion(AxisIndex_t motor_index)
{
  uint8_t axis = axisOf(motor_index);
  if (axis >= motor_nums_)
    return false;

  AxisRuntime_t* rt = &axes_[axis];

  if (rt->pending.mode == OPERATION_MODE_NONE)
    return false;

  rt->current         = rt->pending;
  rt->pending.mode    = OPERATION_MODE_NONE;

  log_debug("Minebea[%d:%d]: Executing motion - mode=%d, target=%d",
            slaveIndex_, axis, rt->current.mode, rt->current.target_position);

  return true;
}

bool MinebeaControl::stopMotion(AxisIndex_t motor_index)
{
  uint8_t axis = axisOf(motor_index);
  if (axis == 0xFF)
  {
    bool re = true;
    for (uint8_t i = 0; i < motor_nums_; i++)
      re = stopMotion((AxisIndex_t)i) && re;
    return re;
  }
  if (axis >= motor_nums_)
    return false;
  //•	Minebea[0:0]：第 0 个从站的第 0 个电机，也就是 1 号轴
  log_info("Minebea[%d:%d]: Stopping motion (3000H=0 减速停)", slaveIndex_, axis);

  axes_[axis].stop_req = 1;

  return true;
}

/// @brief 设置电机运动位置和速度
/// @param motor_index 电机索引
/// @param position 目标位置（单位：mm）
/// @param velocity 目标速度（单位：mm/秒, 只接受正数）
/// @param is_relative 是否为相对位置（true: 相对位置, false: 绝对位置）
bool MinebeaControl::setMotionPosition(AxisIndex_t motor_index, double position, double velocity, bool is_relative)
{
  uint8_t axis = axisOf(motor_index);
  if (axis >= motor_nums_)
    return false;

  MotionCommand_t cmd;
  memset(&cmd, 0, sizeof(cmd));

  uint32_t target_vel = (uint32_t)std::abs(LengthToPulse(velocity));

  if (is_relative)
  {
    cmd.target_position = (int32_t)(MINEBEA_MOTOR_DIR * LengthToPulse(position)); 
    cmd.flag_relative   = true;
  }
  else
  {
    // 绝对定位(3000H=7): 脉冲坐标 = 零点 + 方向 * 换算, 每次一次性截断, 不累积 //根据位移计算脉冲数量  
    cmd.target_position = axes_[axis].zero_pos + (int32_t)(MINEBEA_MOTOR_DIR * LengthToPulse(position));
    cmd.flag_relative   = false;
  }

  cmd.target_velocity  = target_vel;
  cmd.profile_velocity = target_vel;
  cmd.mode             = OPERATION_MODE_POSITION;
  cmd.flag_new_target  = 1;

  log_debug("Minebea[%d:%d]: Set position:%f mm, velocity:%f mm/s, relative=%d, target=%d(pulse)",
            slaveIndex_, axis, position, velocity, is_relative, cmd.target_position);

  return setMotionCommand(motor_index, cmd);  //保存目标参数
}

MotorStatus_t MinebeaControl::getStatus(AxisIndex_t motor_index) const
{
  uint8_t axis = axisOf(motor_index);
  if (axis >= motor_nums_)
  {
    MotorStatus_t empty;
    memset(&empty, 0, sizeof(empty));
    return empty;
  }

  return axes_[axis].ctr.state;
}

void MinebeaControl::printControlStatus(AxisIndex_t motor_index)
{
  uint8_t axis = axisOf(motor_index);
  if (axis >= motor_nums_)
    return;

  const AxisRuntime_t* rt = &axes_[axis];

  std::cout << "Minebea[" << slaveIndex_ << ":" << (int)axis << "]: "
            << "Alarm=" << rt->status.alarmNo << ", "
            << "Pos=" << rt->status.encoderPos << ", "
            << "Dev=" << (rt->current.target_position - rt->status.encoderPos) << ", "
            << "Rpm=" << rt->status.speedRpm << ", "
            << "Target=" << rt->current.target_position << ", "
            << "State=" << static_cast<int>(rt->ctr.work_state) << ", "
            << "Arrived=" << isTargetReached(motor_index)
            << std::endl;
}

bool MinebeaControl::isTargetReached(AxisIndex_t motor_index) const
{
  uint8_t axis = axisOf(motor_index);
  if (axis >= motor_nums_)
    return false;

  // 到位 = 命令块已发到驱动器 且 指令/编码器偏差连续 N 拍在窗口内
  return axes_[axis].cmd_active && (axes_[axis].arrival_cnt >= MINEBEA_ARRIVAL_STABLE);
}

bool MinebeaControl::setParameters(AxisIndex_t motor_index, const std::vector<std::pair<uint16_t, uint32_t>>& params)
{
  uint8_t axis = axisOf(motor_index);
  if (axis >= motor_nums_)
    return false;

  log_debug("Minebea[%d:%d]: Setting %zu parameters", slaveIndex_, axis, params.size());

  for (const auto& param : params)
  {
    switch (param.first)
    {
      case 0x0802:  // 最高速度
        parameters_.profileVelocity = param.second;
        break;
      case 0x0810:  // 加速度(驱动器参数)
        parameters_.profileAcceleration = param.second;
        break;
      case 0x0812:  // 减速度(驱动器参数)
        parameters_.profileDeceleration = param.second;
        break;
      default:
        log_warn("Minebea[%d:%d]: Unknown parameter index 0x%04X", slaveIndex_, axis, param.first);
        break;
    }
  }

  return true;
}

bool MinebeaControl::getParameter(uint16_t index, uint8_t subindex, uint32_t* value)
{
  if (!value)
    return false;

  switch (index)
  {
    case 0x0802:
      *value = parameters_.profileVelocity;
      return true;
    case 0x0810:
      *value = parameters_.profileAcceleration;
      return true;
    case 0x0812:
      *value = parameters_.profileDeceleration;
      return true;
    default:
      log_warn("Minebea: Unknown parameter index 0x%04X", index);
      return false;
  }
}

void MinebeaControl::processPDO(PdoCtl_t& pdoCtl)  //10ms 循环一次 在while(1)里面
{
  static bool bind_err_logged = false;

  if (!m04_.bind(pdoCtl.output, pdoCtl.input, pdoCtl.output_size, pdoCtl.input_size))
  {
    if (!bind_err_logged)
    {
      log_error("Minebea[%d]: PDO 窗口不足或未就绪(out=%zu, in=%zu), 核对 M04_ECAT.cpp 槽位偏移表",
                slaveIndex_, pdoCtl.output_size, pdoCtl.input_size);
      bind_err_logged = true;
    }
    return;
  }
  bind_err_logged = false;

  m04_.processCycle();    // Modbus Enable 保持 + 触发时序推进

  for (uint8_t axis = 0; axis < motor_nums_; axis++)  //逐个处理每一台电机 processAxis(0);processAxis(1);
    processAxis(axis);
}

void MinebeaControl::processAxis(uint8_t axis)
{
  AxisRuntime_t* rt  = &axes_[axis];
  MotorControl_t* ctr = &rt->ctr;

  // 写槽 Rising 触发完成 = 命令块已进入驱动器, 之后的到位判定才有效
  if (m04_.takeWriteDone(axis))
    rt->cmd_active = 1;

  // ① 状态块刷新 + 到位窗口判定
  if (m04_.readStatusBlock(axis, rt->status))  //先读取上一周期状态  获取编码器位置 速度  报警号  实际转矩
  {
    const M04StatusBlock_t& st = rt->status;

    ctr->ethercat_io.iptr_actual_position = (uint32_t)st.encoderPos;
    ctr->ethercat_io.iptr_actual_velocity = (uint32_t)st.speedRpm;
    ctr->ethercat_io.iptr_errorCode       = (uint32_t)st.alarmNo;

    ctr->state.actual_position = MINEBEA_MOTOR_DIR * PulseToLength(st.encoderPos - rt->zero_pos);
    ctr->state.actual_velocity = (double)st.speedRpm / 60.0 * MINEBEA_SCREW_LEAD;
    ctr->state.actual_torque   = (double)st.torqueActual / 1000.0;   // 0.1% 额定 -> 比例
    ctr->state.errorCode       = (uint32_t)st.alarmNo;
    ctr->state.modeOp          = (uint16_t)rt->current.mode;

    // 伪状态字: bit0 运动中, bit1 到位, bit5 伺服关, bit7 报警
    uint32_t sw = 0;
    if (isTargetReached((AxisIndex_t)axis))                       sw |= (1u << 1);
    if (!ctr->flag_enable || ctr->work_state == MOTOR_STATE_DISABLED) sw |= (1u << 5);
    if (st.alarmNo != 0)                                          sw |= (1u << 7);
    if (rt->cmd_active && ctr->work_state == MOTOR_STATE_RUNNING) sw |= (1u << 0);
    ctr->state.statusWord = sw;

    // 到位判定: 目标位置与编码器位置偏差窗口 + 连续稳定周期
    // (3024H 指令位置不在当前读窗内, 用下发目标坐标代替)
    int64_t dev = (int64_t)rt->current.target_position - (int64_t)st.encoderPos;
    if (rt->cmd_active && rt->current.mode == OPERATION_MODE_POSITION &&
        std::llabs(dev) <= MINEBEA_ARRIVAL_WINDOW)
    {
      if (rt->arrival_cnt < 255)
        rt->arrival_cnt++;
    }
    else
    {
      rt->arrival_cnt = 0;
    }

    if (st.alarmNo != rt->last_alarm)
    {
      if (st.alarmNo != 0)
        log_error("Minebea[%d:%d]: Alarm %d", slaveIndex_, axis, st.alarmNo);
      else
        log_info("Minebea[%d:%d]: Alarm cleared", slaveIndex_, axis);
      rt->last_alarm = st.alarmNo;
    }
  }

  // ② 清警/停止请求(写槽空闲时排队, 忙则下拍重试)
  if (rt->clear_req && m04_.requestClearAlarm(axis))
    rt->clear_req = 0;

  if (rt->stop_req && queueStopBlock(axis, MINEBEA_STOP_DECEL))
  {
    rt->stop_req   = 0;
    rt->cmd_active = 0;
    rt->arrival_cnt = 0;
    rt->current.mode         = OPERATION_MODE_NONE;
    rt->current.flag_new_target = 0;
    rt->pending.mode         = OPERATION_MODE_NONE;
    rt->pending.flag_new_target = 0;
    if (ctr->flag_enable)
      ctr->work_state = MOTOR_STATE_WAITTING;
  }

  // ③ 使能状态机: flag_enable=0 时发伺服关; 使能后由 processMotionControl 发伺服开
  if (ctr->flag_enable == 0 && ctr->work_state != MOTOR_STATE_DISABLED)
  {
    if (queueStopBlock(axis, MINEBEA_STOP_SERVO_OFF))
    {
      rt->cmd_active = 0;
      rt->arrival_cnt = 0;
      rt->current.mode = OPERATION_MODE_NONE;
      rt->pending.mode = OPERATION_MODE_NONE;
      ctr->work_state  = MOTOR_STATE_DISABLED;
    }
  }
  else
  {
	  processMotionControl(axis);  //如果电机已经使能,则进入运动状态机
  }

  ctr->state.work_state = ctr->work_state;
}

/**
 * @brief 每轴运动状态机: 伺服开 -> 等命令 -> 下发九字块 -> 运动 -> 到位
 */
void MinebeaControl::processMotionControl(uint8_t axis)
{
  AxisRuntime_t* rt  = &axes_[axis];
  MotorControl_t* ctr = &rt->ctr;
  MotionCommand_t* cmd = &rt->current;

  switch (ctr->work_state)  //初始状态为 MOTOR_STATE_RESET
  {
    case MOTOR_STATE_RESET:
      // 伺服开: 3000H=0 停止 + 3004H=3 Servo-on
      if (queueStopBlock(axis, MINEBEA_STOP_SERVO_ON))
      {
        log_debug("Minebea[%d:%d]: Servo-on sent", slaveIndex_, axis);
        ctr->work_state = MOTOR_STATE_WAITTING;
      }
      break;

    case MOTOR_STATE_WAITTING: //等待 都执行下面的函数
    case MOTOR_STATE_ARRIVED:  //已到位状态
      // 演示模式: 到位后关于零位对称翻转目标
      if (ctr->work_state == MOTOR_STATE_ARRIVED && ctr->work_mode == WORK_MODE_DEMON)
      {
        cmd->target_position = 2 * rt->zero_pos - cmd->target_position;
        cmd->flag_relative   = false;
        cmd->flag_new_target = 1;
      }

      executeMotion((AxisIndex_t)axis);   // 取 pending -> current

      if (cmd->flag_new_target == 1)
      {
        uint16_t op = MINEBEA_OP_POS_ABS;
        int32_t  target = cmd->target_position;
        int32_t  speed  = (cmd->profile_velocity != 0)
                              ? (int32_t)cmd->profile_velocity
                              : std::abs(cmd->target_velocity);

        if (cmd->mode == OPERATION_MODE_VELOCITY)
        {
          op    = MINEBEA_OP_SPEED;
          speed = cmd->target_velocity;   // 速度控制带符号表方向
          target = 0;
        }
        else if (cmd->flag_relative)
        {
          // 相对增量按当前编码器位置换算成绝对目标, 统一走绝对定位,
          // 到位判定与目标显示同一坐标系
          target = rt->status.encoderPos + cmd->target_position;
          cmd->target_position = target;
          cmd->flag_relative   = false;
        }

        if (queueCmdBlock(axis, op, target, speed, MINEBEA_STOP_DECEL))
        {
          cmd->flag_new_target = 0;
          rt->cmd_active  = 0;
          rt->arrival_cnt = 0;
          ctr->work_state = MOTOR_STATE_RUNNING;
        }
      }
      break;

    case MOTOR_STATE_RUNNING:
      // 新指令覆盖旧目标: 驱动器运动中不响应新定位指令, 先减速停, 停稳后转 WAITTING 下发
      if (rt->pending.mode != OPERATION_MODE_NONE)
      {
        if (queueStopBlock(axis, MINEBEA_STOP_DECEL))
        {
          log_info("Minebea[%d:%d]: Override target: decel-stop first (3000H=0)", slaveIndex_, axis);
          rt->stable_cnt = 0;
          ctr->work_state = MOTOR_STATE_STOP;
        }
        break;
      }

      if (!rt->cmd_active)
        break;    // 等写槽触发完成

      if (isTargetReached((AxisIndex_t)axis))
      {
        if (ctr->work_mode == WORK_MODE_INIT)
        {
          log_info("Minebea[%d:%d]: Home point arrived! encoder=%d", slaveIndex_, axis, rt->status.encoderPos);
          ctr->state.flag_offset_init = 1;
          ctr->work_mode = WORK_MODE_MOTOR;
        }
        else
        {
          log_info("Minebea[%d:%d]: Target reached, encoder=%d, dev=%d",
                   slaveIndex_, axis, rt->status.encoderPos,
                   (rt->current.target_position - rt->status.encoderPos));
        }
        ctr->work_state = MOTOR_STATE_ARRIVED;
      }
      break;

    case MOTOR_STATE_STOP:
      // 换目标减速停中: 编码器连续 MINEBEA_ARRIVAL_STABLE 拍(50ms)不动才算停稳,
      // 停稳后转 WAITTING 把 pending 当新目标下发
      if (rt->last_pos == rt->status.encoderPos)
      {
        if (++rt->stable_cnt >= MINEBEA_ARRIVAL_STABLE)
        {
          rt->stable_cnt = 0;
          ctr->work_state = MOTOR_STATE_WAITTING;
        }
      }
      else
      {
        rt->stable_cnt = 0;
      }
      rt->last_pos = rt->status.encoderPos;
      break;

    default:
      break;
  }
}

bool MinebeaControl::queueCmdBlock(uint8_t axis, uint16_t op, int32_t target, int32_t speed, uint16_t stopOpt)
{
  uint16_t reg9[9] = {0};

  reg9[0] = op;                                     // 3000H Operation order
  reg9[1] = 0;                                      // 3001H Direction order(定位不用)
  reg9[2] = (uint16_t)(((uint32_t)speed) >> 16);    // 3002H 速度高
  reg9[3] = (uint16_t)((uint32_t)speed);            // 3003H 速度低
  reg9[4] = stopOpt;                                // 3004H Stop Option
  reg9[5] = 0;                                      // 3005H 程序号(不用)
  reg9[6] = (uint16_t)(((uint32_t)target) >> 16);   // 3006H 目标位置高
  reg9[7] = (uint16_t)((uint32_t)target);           // 3007H 目标位置低
  reg9[8] = 0;                                      // 3008H 转矩(不用)

  return m04_.writeCmdBlock(axis, reg9);
}

bool MinebeaControl::queueStopBlock(uint8_t axis, uint16_t stopOpt)
{
  return queueCmdBlock(axis, MINEBEA_OP_STOP, 0, 0, stopOpt);
}

int32_t MinebeaControl::LengthToPulse(double len) const
{
  double pulse = (double)MINEBEA_PULSE_PER_REV * (len * MINEBEA_RATIO / MINEBEA_SCREW_LEAD);
  return (int32_t)pulse;
}

double MinebeaControl::PulseToLength(int32_t pulse) const
{
  return (double)MINEBEA_SCREW_LEAD * ((double)pulse / MINEBEA_PULSE_PER_REV / MINEBEA_RATIO);
}

uint8_t MinebeaControl::axisOf(AxisIndex_t index) const
{
  switch (index)
  {
    case Axis_Index_1: return 0;
    case Axis_Index_2: return 1;
    case Axis_Index_3: return 2;
    default:           return 0xFF;
  }
}
