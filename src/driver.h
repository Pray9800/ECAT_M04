#ifndef DRIVER_H
#define DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif
#include "soem/soem.h"
#ifdef __cplusplus
}
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  typedef enum {
    MOTOR_TYPE_UNKNOWN = 0,
    MOTOR_TYPE_STF05_4XU,      // 鸣志  4通道电机驱动器
    MOTOR_TYPE_MS_AK06,      //鸣志 单通道电机驱动器
    MOTOR_TYPE_TSJA20,     // 天链
    MOTOR_TYPE_M04,        // 美蓓亚 经M04网关Modbus RTU
} MotorType_t;


  typedef enum
  {
    Axis_Index_1       = 0,
    Axis_Index_2       = 1,
    Axis_Index_3       = 2,
    Axis_Index_4       = 3,
    Axis_Index_All     = 0xF0,
    Axis_Index_Unknown = 0xFF
  } AxisIndex_t;

  typedef struct _stswrd_bits_t
  {
    uint8_t rtso : 1;
    uint8_t so : 1;
    uint8_t oe : 1;
    uint8_t f : 1;
    uint8_t ve : 1;
    uint8_t qs : 1;
    uint8_t sod : 1;
    uint8_t w : 1;
    uint8_t ms1 : 1;
    uint8_t rm : 1;
    uint8_t tr : 1;
    uint8_t ila : 1;
    uint8_t oms1 : 1;
    uint8_t oms2 : 1;
    uint8_t ms2 : 1;
    uint8_t ms3 : 1;
  } stswrd_bits_t;

  typedef union _stswrd_t
  {
    uint16_t      all;
    stswrd_bits_t bit;
  } stswrd_t;

#ifdef _MSC_VER
  #define PACKED
  #pragma pack(push, 1)
#else
  #define PACKED __attribute__((packed))
#endif

  typedef struct PACKED
  {
    uint16_t ControlWord;  // 0x6040
    int32_t  TargetPos;    // 0x607A
    int32_t  TargetVel;    // 0x60FF
    uint32_t ProfileVel;   // 0x6081
    uint32_t ProfileAcce;   //0x6083
    uint32_t ProfileDece;   //0x6084
    uint8_t ModeOp;  // 0x6060
  } Drive_Outputs_t;

  typedef struct PACKED
  {
    uint16_t StatusWord;     // 0x6041
    int32_t  ActualPos;      // 0x6064
    int32_t  ActualVel;      // 0x066C
    uint8_t  ModeOp;         // 0x6061
    uint32_t DigitalInputs;  // 0x60FD
  } Drive_Inputs_t;


   typedef struct PACKED
  {
    uint16_t ControlWord;  // 0x6040
    int32_t  TargetPos;    // 0x607A
    int32_t  TargetVel;    // 0x60FF
    uint16_t ModeOp;       // 0x6060
    int16_t TargetTorque;  //0x6071
    int16_t TorqueOffset;  //0x60B2
  } TSJA20_Outputs_t;

  typedef struct PACKED
  {
    uint16_t StatusWord;     // 0x6041
    int32_t  ActualPos;      // 0x6064
    int32_t  ActualVel;      // 0x066C
    uint16_t  ModeOp;         // 0x6061
    int16_t  ActualTorque;     //0x6077
    // uint16_t ErrCode;        //0x603F
  } TSJA20_Inputs_t;


#ifdef _MSC_VER
  #pragma pack(pop)
#endif

  void delay_ms(int milliseconds);

  void driver_read_CSP_SDO(ecx_contextt *context, uint16_t slave);

  int  drive_setup(ecx_contextt *context, uint16 slave);
  int drive_setup_ms_ak06(ecx_contextt *context, uint16 slave);
  int drive_setup_tsja(ecx_contextt *context, uint16 slave);

  int drive_slave_setup(ecx_contextt *context, uint16 slave, MotorType_t motorType);


#ifdef __cplusplus
}
#endif

#endif  // DRIVER_H
