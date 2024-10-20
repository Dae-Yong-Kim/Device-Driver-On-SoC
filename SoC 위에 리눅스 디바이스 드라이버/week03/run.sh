#!/bin/bash
qemu-8.0.5/build/qemu-system-aarch64 -kernel linux-6.5.5/arch/arm64/boot/Image -append "root=/dev/mmcblk0p1 console=ttyAMA0 rootwait" -nographic -M comento -m 1G -smp 2 -dtb linux-6.5.5/arch/arm64/boot/dts/comento/comento.dtb -drive format=raw,file=sdcard.img,if=sd  -qmp unix:/tmp/qmp.sock,server,nowait
