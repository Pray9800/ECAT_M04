#ifndef MOTORCONTROLLER_H
#define MOTORCONTROLLER_H

#include <cstdint>
#include <vector>
#include <utility>
#include <stdint.h>
#include <time.h>
#include <chrono>
#include <thread>

#include "driver.h"



typedef enum{
    SLAVE_INDEX_1,
    SLAVE_INDEX_2,
    SLAVE_INDEX_3,
    SLAVE_INDEX_4,
    SLAVE_INDEX_5,
    SLAVE_INDEX_6,
}SlaveIndex_t;


/// @brief 运动模式枚举
typedef enum {
    OPERATION_MODE_NONE = 0,            // 无运动模式
    OPERATION_MODE_POSITION = 1,        // 轮廓位置模式
    OPERATION_MODE_VELOCITY = 3,        // 轮廓速度模式
    OPERATION_MODE_HOMING = 6,          // 原点模式
    OPERATION_MODE_CSP = 8,             // 周期同步位置模式
    OPERATION_MODE_CSV = 9,             // 周期同步速度模式
    OPERATION_MODE_CST = 10             // 周期同步扭矩模式
} OperationMode_t;

/// @brief 电机使能枚举
typedef enum
{
  MOTOR_DISABLE = 0,
  MOTOR_ENABLE  = 1
} MotorEnable_t;


/// @brief 工作模式枚举
typedef enum
{
  WORK_MODE_NONE,
  WORK_MODE_SET_ZERO,       //设置零位模式  电机处于0力矩模式    
  WORK_MODE_INIT,           //初始化模式  电机回零位
  WORK_MODE_MOTOR,          //电机运行模式
  WORK_MODE_MOTOR_STOP,
  WORK_MODE_DEMON
} WorkMode_t;


/// @brief 电机状态枚举
typedef enum
{
    MOTOR_STATE_RESET,
    MOTOR_STATE_INIT,
    MOTOR_STATE_WAITTING,
    MOTOR_STATE_ENABLED,
    MOTOR_STATE_RUNNING,
    MOTOR_STATE_ARRIVED,
    MOTOR_STATE_STOP,
    MOTOR_STATE_DISABLED,
    // MOTOR_STATE_FAULT
} WorkState_t;


typedef enum
{
    STATE_STANDBY,
    STATE_RESET,
    STATE_INIT,
    STATE_PREREADY,
    STATE_READY,
    STATE_PREENABLE,
    STATE_ENABLE,
    STATE_DISABLE,
    STATE_FAULT,
}State_t;




typedef struct
{
  std::time_t t0;
  std::time_t t1;
  std::time_t span;
  int         flag_start;
  int         count;
} TimeSpan_t;



typedef struct
{
    //电机运行参数 ethercat output input
    uint32_t iptr_actual_position;  //当前位置 count
    uint32_t iptr_actual_velocity;  //当前速度 count/s
    uint32_t iptr_actual_torque;    //当前扭矩 count
    uint32_t iptr_digital_inputs;   //数字输入
    uint32_t iptr_statusWord;       //状态字
    uint32_t iptr_modeOp;           //运动模式
    uint32_t iptr_errorCode;        //错误码

    uint32_t optr_target_position;  //目标位置 count
    uint32_t optr_target_velocity;  //目标速度 count/s
    uint32_t optr_profile_velocity;  //轮廓速度 count/s
    uint32_t optr_profile_acceleration;  //轮廓加速度 count/s/s
    uint32_t optr_profile_deceleration;  //轮廓减速度 count/s/s/s
    uint32_t optr_target_torque;    //目标扭矩 count
    uint32_t optr_modeOp;           //运动模式
    uint32_t optr_controlWord;      //控制字
}EthercatIO_t;




/// @brief 电机状态结构体
typedef struct {

    WorkState_t work_state;
    uint32_t digital_inputs;
    uint32_t statusWord;
    // uint32_t controlWord;
    uint16_t modeOp;
    uint32_t errorCode;
    uint8_t flag_offset_init;
    // int32_t target_position;

    double cycle_time;


    //电机输出
    double actual_position;     //当前位置 用户单位：度或毫米  已进行零点修正
    double actual_velocity;     //当前速度 用户单位：度或毫米/秒
    double actual_torque;       //当前扭矩 用户单位：Nm
    double torque_offset;       //当前扭矩偏移 用户单位：Nm
    double position_zero_offset;

} MotorStatus_t;


/// @brief 运动命令结构体
typedef struct {
    int32_t target_position;
    int32_t target_velocity;
    int32_t target_torque;
    OperationMode_t mode;
    uint32_t profile_velocity;
    uint32_t profile_acceleration;
    uint32_t profile_deceleration;

    uint8_t flag_new_target;
    bool flag_relative;      // 是否相对运动

} MotionCommand_t;

/// @brief 运动参数结构体
typedef struct {
    uint16_t index;
    uint32_t profile_velocity;
    uint32_t profile_acceleration;
    uint32_t profile_deceleration;
} MotionParameter_t;

/// @brief 运动控制结构体
typedef struct{

    WorkMode_t work_mode;
    WorkState_t work_state;
    uint8_t flag_enable;
    uint32_t time_count;
    MotorStatus_t state;
    uint32_t errorCode;

    uint32_t ccnt;
    uint32_t enable_wait_cnt;

    TimeSpan_t command_delay;

    //步进电机 初始化参数
    int     steps_offset;
    int     pos_with_offset;
    // uint8_t flag_offset_init;

    EthercatIO_t ethercat_io;

} MotorControl_t;


typedef struct{
    uint32_t cycle_ns;
    uint8_t* input;
    uint8_t* output;
    size_t input_size;
    size_t output_size;
}PdoCtl_t;


/// @brief 电机参数结构体（初始化后不可修改）
/// 
/// 包含电机的物理参数，用于单位转换和运动约束
/// 这些参数必须在电机初始化时设置，设置后不允许修改
typedef struct {
    uint32_t encoderResolution;  ///< 电机转一圈的编码器值（脉冲数/转）
    double gearRatio;            ///< 减速比（电机转速/输出轴转速，>1表示减速）
    double maxPosition;          ///< 最大位置限制（用户单位：度或毫米）
    double minPosition;          ///< 最小位置限制（用户单位：度或毫米）
    double maxVelocity;          ///< 最大速度限制（用户单位/秒）
    double maxAcceleration;      ///< 最大加速度限制（用户单位/秒²）
    double maxJerk;              ///< 最大加加速度限制（用户单位/秒³）

    double dir;                  ///< 电机方向（1表示正转，-1表示反转）

} MotorConfig_t;





class MotorController {
public:
    MotorController() : paramsInitialized_(false) {}
    virtual ~MotorController() = default;
    
    // 初始化
    virtual bool init(uint16_t slaveIndex, uint8_t motor_nums, int32_t *zero_pos) = 0;
    virtual bool setupPDO() = 0;
    virtual int getMotorNum() const = 0;
    

    // 获取电机参数
    virtual MotorConfig_t getMotorConfig() const = 0;
    
    // 控制接口
    virtual bool enable(AxisIndex_t motor_index) = 0;
    virtual bool disable(AxisIndex_t motor_index) = 0;
    virtual bool reset(AxisIndex_t motor_index) = 0;
    
    // 运动控制
    virtual bool setMotionCommand(AxisIndex_t motor_index, const MotionCommand_t& cmd) = 0;     //设置运动参数
    virtual bool setMotionParameter(AxisIndex_t motor_index, const MotionParameter_t& param) = 0; //设置运动参数
    virtual bool setMotionWorkMode(AxisIndex_t motor_index, WorkMode_t w_mode) = 0; //设置工作模式
    virtual bool executeMotion(AxisIndex_t motor_index) = 0;
    virtual bool stopMotion(AxisIndex_t motor_index) = 0;

    //位置控制
    /// @brief 设置电机运动位置和速度
    /// @param motor_index 电机索引
    /// @param position 目标位置（单位：用户单位）
    /// @param velocity 目标速度（单位：用户单位/秒, 只接受正数）
    /// @param is_relative 是否为相对位置（true: 相对位置, false: 绝对位置）
    /// @return 
    virtual bool setMotionPosition(AxisIndex_t motor_index, double position, double velocity, bool is_relative) = 0;
    
    // 状态获取
    virtual MotorStatus_t getStatus(AxisIndex_t motor_index) const = 0;
    virtual void printControlStatus(AxisIndex_t motor_index) = 0;
    virtual bool isTargetReached(AxisIndex_t motor_index) const = 0;
    
    // 参数配置
    virtual bool setParameters(AxisIndex_t motor_index, const std::vector<std::pair<uint16_t, uint32_t>>& params) = 0;
    virtual bool getParameter(uint16_t index, uint8_t subindex, uint32_t* value) = 0;
    
    // PDO处理
    virtual void processPDO(PdoCtl_t& pdoCtl) = 0;

    
    // 信息
    virtual MotorType_t getType() const = 0;
    virtual const char* getName() const = 0;
    
protected:
    SlaveIndex_t slaveIndex_;
    uint8_t motor_nums_;
    
    MotorConfig_t motorParams_;   ///< 电机参数（初始化后不可修改）
    bool paramsInitialized_;      ///< 参数是否已初始化（防止重复设置）
};


#endif
