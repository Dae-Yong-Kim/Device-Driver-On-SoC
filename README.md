# Device-Driver-On-SoC
Porting Linux Device Driver On SoC

## 환경
- linux(Docker)
- qemu

## 환경 설정
1. 도커 설치
```
https://docs.docker.com/get-started/get-docker/
```
2. WSL 버전 업데이트
Docker Desktop - WSL kernel version too low 메세지가 나온다면 cmd에서
```
wsl --update
```
3. Docker 실행
```
docker run -dit --name comento ubuntu:22.04 //comento 컨테이너 만들기
docker attach comento //컨테이너 접속
docker start comento //컨테이너 다시 시작하기
docker exec -it comento /bin/bash //새로운 쉘 더 띄우기
```
4. 커널, 빌드루트, QEMU 다운로드
컨테이너 기본 환경 설정
```
apt-get update
apt-get install sudo wget vim xz-utils
```
커널 빌드 루트, QEMU 다운로드
```
cd ~
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.5.5.tar.xz
wget https://buildroot.org/downloads/buildroot-2023.08.tar.gz
wget https://download.qemu.org/qemu-8.0.5.tar.xz
tar xvf linux-6.5.5.tar.xz
tar xvf buildroot-2023.08.tar.gz
tar xvf qemu-8.0.5.tar.xz
```
5. 빌드루트 빌드하기
```
sudo apt install build-essential file bison flex cpio unzip rsync bc libncurses-dev \
 --no-install-recommends //빌드 의존성 패키지 설치
cd buildroot-2023.08; make qemu_aarch64_virt_defconfig; make menuconfig //빌드 루트 설정
	• Toolchain -> Toolchain type : External toolchain
	• Filesystem images -> cpio the root filesystem : 선택
	• Filesystem images -> cpio the root filesystem -> Compression method : gzip
	• Kernel -> Linux kernel : 선택 안함
	• Host utilities : 모든 항목 선택 안함
make -j<코어 개수> //빌드 루트 빌드
```
6. 커널 빌드하기
```
sudo apt install clang lld llvm --no-install-recommends //툴체인 대신 LLVM 사용
cd linux-6.5.5
cp ../buildroot-2023.08/board/qemu/aarch64-virt/linux.config arch/arm64/configs/comento_defconfig //나중에 디바이스 트리에서 사용
ARCH=arm64 LLVM=1 make comento_defconfig //커널 설정
ARCH=arm64 LLVM=1 make –j<코어 개수> //커널 빌드
```
7. QEMU 빌드
```
sudo apt install meson ninja-build pkg-config libglib2.0-dev libpixman-1-dev \
 --no-install-recommends //빌드 의존성 패키지 설치
cd qemu-8.0.5
mkdir build; cd build; ../configure --target-list="aarch64-softmmu" --without-default-features //QEMU 설정
cd build; make –j<코어 개수> //QEMU 빌드
```
8. 환경 테스트
```
<QEMU 디렉토리>/build/qemu-system-aarch64 \
 -kernel <리눅스 디렉토리>/arch/arm64/boot/Image \
 -drive format=raw,file=<빌드루트 디렉토리>/output/images/rootfs.ext4,if=virtio \
 -append "root=/dev/vda console=ttyAMA0" \
 -nographic -M virt -cpu cortex-a72 \
 -m 2G \
 -smp 2
```
Welcome to Buildroot 까지 뜨면 성공!
• 로그인 ID : root, 비밀번호 : 없음
• QEMU 종료하기 : poweroff -f

## 목표
![최종 목표](https://github.com/Dae-Yong-Kim/Device-Driver-On-SoC/blob/main/readmefile_image/%EC%B5%9C%EC%A2%85%20%EB%AA%A9%ED%91%9C.jpg)

## QEMU에 소스 코드 추가
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
CONFIG_COMENTO=y
```
3. qemu-8.0.5/hw/arm/meson.build에 추가 //추가할 소스 코드(~~~.c)가 빌드되도록 설정
```
arm_ss.add(when: 'CONFIG_COMENTO', if_true: files('comento.c'))
```
4. qemu-8.0.5/hw/arm/에 새로운 소스 코드(~~~.c) 추가
```
레파지토리 확인 (comento.c, kdy.c)
```
5. QEMU 새로 빌드
```
cd qemu-8.0.5/build; make -j<코어 개수>
```
6. 


## 새로운 디바이스 트리 추가
1. linux-6.5.5/arch/arm64/Kconfig.platforms에 추가
```
config ARCH_COMENTO
	bool "Comento SoC"
	help
	  This enables support for Comento Soc family
```
2.linux-6.5.5/arch/arm64/boot/dts/Makefile에 추가 + 새 디렉토리 추가
```
mkdir linux-6.5.5/arch/arm64/boot/dts/comento
subdir-y += comento
```
3. linux-6.5.5/arch/arm64/boot/dts/comento/Makefile에 추가
```
dtb-$(CONFIG_ARCH_COMENTO) += comento.dtb
```
4. 커널 빌드할 때 추가했던 defconfig에 추가한 config(linux-6.5.5/arch/arm64/configs/comento_defconfig
)에 추가
```
CONFIG_ARCH_COMENTO=y //Makefile에서 사용
CONFIG_BLK_DEV_INITRD=y //Initramfs를 사용을 위한 설정
CONFIG_RD_GZIP=y //Initramfs를 사용을 위한 설정
```
5. linux-6.5.5/arch/arm64/boot/dts/comento/comento.dts 추가
```
레파지토리 확인 (comento.dts, kdy.dts)
```
7. linux-6.5.5/에서 defconfig 적용
```
ARCH=arm64 make comento_defconfig
ARCH=arm64 LLVM=1 make –j<코어 개수> //커널 빌드
```
