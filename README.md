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
docker run --privileged -dit -v /dev:/dev -v C:\study\my_shared_directory:/root --name comento ubuntu:22.04 //comento 컨테이너 만들기
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
make –j<코어 개수> //QEMU 빌드
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

# Week01
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
week01 레파지토리 확인 (comento.c, kdy.c)
```
5. QEMU 새로 빌드
```
cd qemu-8.0.5/build; make -j<코어 개수>
```

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
week01 레파지토리 확인 (comento.dts, kdy.dts)
```
7. linux-6.5.5/에서 defconfig 적용
```
ARCH=arm64 make comento_defconfig
ARCH=arm64 LLVM=1 make –j<코어 개수> //커널 빌드
```

## QEMU실행
```
<QEMU 디렉토리/build/qemu-system-aarch64 \
-kernel <리눅스 디렉토리>/arch/arm64/boot/Image \
-initrd <빌드루트 디렉토리>/output/images/rootfs.cpio.gz \
-append "console=ttyAMA0“ \
-dtb <리눅스 디렉토리>/arch/arm64/boot/dts/comento/comento.dtb \
-nographic -M comento -m 1G -smp 2
```
# Week02
## QEMU에 MMIO 하드웨어 추가
1. qemu-8.0.5/hw/misc/에 comento 디렉토리 만들기 + qemu-8.0.5/hw/misc/meson.build 맨 아래에 추가
```
subdir('comento')
```
2. qemu-8.0.5/hw/misc/comento/meson.build
```
softmmu_ss.add(when: 'CONFIG_COMENTO', if_true: files('mmio.c'))
```
3. qemu-8.0.5/hw/misc/comento/mmio.c 추가
```
week02 레파지토리 확인 (mmio.c)
```
4. qemu-8.0.5/hw/arm/comento.c에 추가
```
week02 레파지토리 확인 (comento.c)
```
5. QEMU 새로 빌드
```
cd qemu-8.0.5/build; make -j<코어 개수>
```
## MMIO 다바이스 트리, 드라이버 추가
1. 디바이스 트리에 MMIO 하드웨어 추가(linux-6.5.5/arch/arm64/boot/dts/comento/comento.dts)
```
week02 레파지토리 확인 (comento.dts)
```
2. 새로운 드라이버 추가하기
	2-1. linux-6.5.5/drivers/comento 디렉토리 생성 + linux-6.5.5/drivers/comento/Kconfig에 다음 추가
```
config COMENTO_DRIVER
	bool "Comento SoC device drivers"
	help
		This enables device drivers for Comento SoC

```
	2-2. linux-6.5.5/drivers/comento/Makefile에 다음 추가
```
obj-y += mmio.o
```
	2-3. linux-6.5.5/drivers/Kconfig에 다음 추가
```
source "drivers/comento/Kconfig"
```
	2-4. linux-6.5.5/drivers/Makefile에 다음 추가
```
obj-$(CONFIG_COMENTO_DRIVER) += comento/
```
	2-5. linux-6.5.5/arch/arm64/configs/comento_defconfig에 다음 추가
```
CONFIG_COMENTO_DRIVER=y
```
	2-6. linux-6.5.5/에서 다음 명령어 실행
```
ARCH=arm64 LLVM=1 make comento_defconfig
```
3. linux-6.5.5/drivers/comento/mmio.c 추가
```
week02 레파지토리 확인 (mmio-driver.c)
```
4. linux-6.5.5/에서 빌드
```
ARCH=arm64 LLVM=1 make -j32
```
## 작동 확인
1. QEMU 실행시 QMP를 보내기 위한 소켓을 추가
```
qemu-8.0.5/build/qemu-system-aarch64 -kernel linux-6.5.5/arch/arm64/boot/Image -initrd buildroot-2023.08/output/images/rootfs.cpio.gz -append "console=ttyAMA0" -dtb linux-6.5.5/arch/arm64/boot/dts/comento/comento.dtb -nographic -M comento -m 1G -smp 2 -qmp unix:/tmp/qmp.sock,server,nowait
```
2. QMP 스크립트를 사용
```
cd qemu-8.0.5/scripts/qmp
목록 조회 : qemu/scripts/qmp/qom-list --socket /tmp/qmp.sock /machine/peripheral/
속성 읽기 : ./qom-get --socket /tmp/qmp.sock /machine/peripheral/mmio-comento.data
속성 쓰기 : ./qom-set --socket /tmp/qmp.sock /machine/peripheral/mmio-comento.data "문자열"
```
3. 리눅스에서 /dev/comento-mmio0를 통해 송수신
```
cat /dev/comento-mmio0
echo "문자열" > /dev/comento-mmio0
```
# Week03
## DMA 추가
1-1. qemu-8.0.5/hw/arm/comento.c에 추가 (DMA 하드웨어 추가 & DMA에 신호처리 연결)
```
week03 레파지토리 확인 (comento.c)
```
1-2. QEMU 새로 빌드
```
cd qemu-8.0.5/build; make -j32
```
2-1. linux-6.5.5/arch/arm64/configs/comento_defconfig에 다음 추가 (DMA 디바이스 드라이버 사용)
```
CONFIG_DMADEVICES=y
CONFIG_DMA_ENGINE=y
CONFIG_PL330_DMA=y
```
3-1. 디바이스 트리에 DMA 하드웨어 추가(linux-6.5.5/arch/arm64/boot/dts/comento/comento.dts)
```
week03 레파지토리 확인 (comento.dts)
```
3-2. Linux 새로 빌드
```
cd linux-6.5.5
ARCH=arm64 LLVM=1 make comento_defconfig
ARCH=arm64 LLVM=1 make -j32
```
## MMIO 하드웨어, 드라이버, 다바이스 트리 수정
1-1. qemu-8.0.5/hw/misc/comento/mmio.c 추가 (MMIO 하드웨어 수정)
```
week03 레파지토리 확인 (mmio.c)
```
1-2. 빌드
```
cd qemu-8.0.5/build; make -j32
```
2-1. linux-6.5.5/drivers/comento/mmio.c 추가 (MMIO 드라이버 수정)
```
week03 레파지토리 확인 (mmio-driver.c)
```
3-1. linux-6.5.5/arch/arm64/boot/dts/comento/comento.dts 추가 (MMIO 디바이스 트리 수정)
```
week03 레파지토리 확인 (comento.dts)
```
3-2. 빌드
```
cd linux-6.5.5
ARCH=arm64 LLVM=1 make comento_defconfig
ARCH=arm64 LLVM=1 make -j32
```
## 작동 확인
1. QEMU 실행시 QMP를 보내기 위한 소켓을 추가
```
qemu-8.0.5/build/qemu-system-aarch64 -kernel linux-6.5.5/arch/arm64/boot/Image -initrd buildroot-2023.08/output/images/rootfs.cpio.gz -append "console=ttyAMA0" -dtb linux-6.5.5/arch/arm64/boot/dts/comento/comento.dtb -nographic -M comento -m 1G -smp 2 -qmp unix:/tmp/qmp.sock,server,nowait
```
2. QMP 스크립트를 사용
```
cd qemu-8.0.5/scripts/qmp
목록 조회 : qemu/scripts/qmp/qom-list --socket /tmp/qmp.sock /machine/peripheral/
속성 읽기 : ./qom-get --socket /tmp/qmp.sock /machine/peripheral/mmio-comento.data
속성 쓰기 : ./qom-set --socket /tmp/qmp.sock /machine/peripheral/mmio-comento.data "문자열"
```
3. 리눅스에서 /dev/comento-mmio0를 통해 송수신
```
cat /dev/comento-mmio0
echo "문자열" > /dev/comento-mmio0
```
## MMC 추가
1-1. qemu-8.0.5/hw/arm/comento.c에 추가 (MMC 하드웨어 추가)
```
week03 레파지토리 확인 (comento.c)
```
1-2. QEMU 새로 빌드
```
cd qemu-8.0.5/build; make -j32
```
2-1. linux-6.5.5/arch/arm64/configs/comento_defconfig에 다음 추가 (MMC 디바이스 드라이버 사용)
```
CONFIG_MMC=y
CONFIG_MMC_ARMMMCI=y
// PMIC로 사용하는 regulator-fixed 관련 설정
CONFIG_REGULATOR=y
CONFIG_REGULATOR_FIXED_VOLTAGE=y
```
3-1. 디바이스 트리에 MMC 하드웨어 추가(linux-6.5.5/arch/arm64/boot/dts/comento/comento.dts)
```
week03 레파지토리 확인 (comento.dts)
```
3-2. Linux 새로 빌드
```
cd linux-6.5.5
ARCH=arm64 LLVM=1 make comento_defconfig
ARCH=arm64 LLVM=1 make -j32
```
## SD카드 이미지 생성하기 (~/에서 실행)
1. 목표로하는 크기의 0으로 채워진 이미지 파일 생성
```
dd if=/dev/zero of=sdcard.img count=1 bs=64M
```
2. 이미지 파일에 파티션 정보를 추가
```
sudo apt-get install fdisk
sudo apt-get update
fdisk sdcard.img
• n<엔터> p<엔터> 1<엔터> <엔터> <엔터> w<엔터>
```
3. 이미지 파일을 Loop 디바이스로 설정
```
sudo losetup -Pf --show sdcard.img
```
4. Loop 디바이스의 첫 번째 파티션을 ext4 형식으로 포맷
```
sudo mkfs.ext4 <loop 디바이스 경로>p1
```
5. 포맷한 파티션과 빌드루트에서 생성된 이미지 동시에 마운트
```
mkdir mnt1 mnt2
sudo mount -o loop <loop 디바이스 경로>p1 mnt1
sudo mount -o loop <빌드루트 디렉토리>/output/images/rootfs.ext4 mnt2
```
6. 마운트된 빌드루트 이미지의 내용을 마운트된 포맷 파티션으로 복사
```
sudo cp -R mnt2/* mnt1/.
```
7. 마운트한 디렉토리를 모두 언마운트
```
sync; sudo umount mnt1 mnt2
```
8. Loop 디바이스 해제
```
sudo losetup -d <loop 디바이스 경로>
```
## 작동 확인
1. QEMU 실행시 initrd 사용 X
```
qemu-8.0.5/build/qemu-system-aarch64 -kernel linux-6.5.5/arch/arm64/boot/Image -drive format=raw,file=sdcard.img,if=sd -append "root=/dev/mmcblk0p1 console=ttyAMA0 rootwait" -dtb linux-6.5.5/arch/arm64/boot/dts/comento/comento.dtb -qmp unix:/tmp/qmp.sock,server,nowait -nographic -M comento -m 1G -smp 4
```
