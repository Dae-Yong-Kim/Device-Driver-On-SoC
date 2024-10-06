# Device-Driver-On-SoC
Porting Linux Device Driver On SoC

## 환경
- linux(Docker)
 - sl 
- qemu

## 목표


## QEMU 설정
1. qemu-8.0.5/hw/arm/Kconfig에 추가
```
config COMENTO
 bool
 imply I2C_DEVICES
 select ARM_GIC
 select PL011 # UART
 select PL031 # RTC
 select PL181 # mmc
 select PL330 # dma
 select PL022 # SPI
 select PL061 # GPIO
 select ARM_SBCON_I2C # I2C
```
2. qemu-8.0.5/configs/devices/arm-softmmu/default.mak에 추가 //추가한 config 적용
```
CONFIG_
```
3. qemu-8.0.5/hw/arm/meson.build에 추가 //추가할 소스 코드(~~~.c)가 빌드되도록 설정
```
arm_ss.add(when: 'CONFIG_COMENTO', if_true: files('comento.c'))
```
4. 새로운 소스 코드(~~~.c) 추가
```
레파지토리 확인 (comento.c, kdy.c)
```
