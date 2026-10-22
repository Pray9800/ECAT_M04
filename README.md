# M04 EtherCAT 设备测试程序

## 测试目标
测试 EC2-MB-M04 设备的EtherCAT通讯。

## 前置条件

### 1. 硬件连接
- M04设备上电（电源灯亮）
- 网线连接电脑和M04
- 不要连接其他EtherCAT设备（单独测试）

### 2. 获取网卡名称
打开 PowerShell（管理员），运行：
```powershell
ipconfig /all
```
找到你的EtherCAT网卡，记录GUID，格式如：
```
{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
```

完整的网卡名称格式：
```
\Device\NPF_{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
```

## 编译步骤

```powershell
# 进入测试目录
cd e:\JT-021\BEIJING_ECAT\test_M04

# 创建build目录
mkdir build
cd build

# 生成VS工程
cmake .. -G "Visual Studio 18 2026"

# 编译
cmake --build . --config Release
```

## 运行测试

```powershell
# 以管理员身份运行（必须！）
.\bin\Release\test_m04.exe "\Device\NPF_{你的网卡GUID}"
```

## 预期结果

### 成功情况
```
=== M04 EtherCAT 设备测试程序 ===
==================================

使用网卡: \Device\NPF_{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}

正在初始化SOEM...
SOEM初始化成功!

正在扫描EtherCAT从站...
找到 1 个从站

=== 从站 1 信息 ===
名称: EC2-MB-M04
厂商ID: 0x00000B95
产品代码: 0x00001410
版本号: 0x00010100
...
>>> 这是 AMSAMOTION 设备！
>>> 这是 EC2-MB-M04 设备！

=== 通讯测试成功! ===
```

### 失败情况

#### 未找到从站
```
未找到任何EtherCAT从站!
```
**解决方法:**
1. 检查网线连接
2. 检查M04电源
3. 检查网卡名称是否正确
4. 确认已安装Npcap

#### SOEM初始化失败
```
SOEM初始化失败!
```
**解决方法:**
1. 以管理员身份运行
2. 检查网卡名称
3. 关闭Wireshark等抓包软件
4. 重新安装Npcap

## 文件说明

- `test_m04.cpp` - 测试程序源码
- `CMakeLists.txt` - CMake构建配置
- `soem/` - SOEM库文件
- `AMX_EC2_MB_M04_general_0.1_1.0.xml` - M04设备ESI文件

## 故障排查

### 1. 确认Npcap安装
```powershell
# 检查Npcap服务
Get-Service -Name npcap
```

### 2. 检查网卡
```powershell
# 列出所有网卡
Get-NetAdapter | Format-Table Name, InterfaceDescription, Status
```

### 3. 检查M04设备
- 电源指示灯是否亮
- 网口指示灯是否闪烁
- 尝试更换网线

### 4. 权限问题
```powershell
# 以管理员身份打开PowerShell
Start-Process powershell -Verb RunAs
```

## 联系支持

如果问题仍未解决：
1. 截图错误信息
2. 记录M04指示灯状态
3. 记录网卡信息