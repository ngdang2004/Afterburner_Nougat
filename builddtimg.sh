#!/bin/bash

KERNELDIR=/home/brett/j7elte/j7nougatkernel/abnougat
MYOUT=$KERNELDIR/arch/arm64/boot
ABDIR=$KERNELDIR/afterburner
MYTOOLS=$ABDIR/mkdtbhbootimg/bin

# copy a temp ramdisk to make a temp boot image
cp $ABDIR/ramdisk/boot.img-ramdisk.gz $MYOUT/

cd $MYOUT
mkdir $MYOUT/j7e3g

# Compile the dt for J7 3g as its the only one that works correctly
cp dts/exynos7580-j7e3g_rev00.dtb j7e3g/
cp dts/exynos7580-j7e3g_rev05.dtb j7e3g/
cp dts/exynos7580-j7e3g_rev08.dtb j7e3g/


# a workaround to get the dt.img for j7e3g
$MYTOOLS/mkbootimg --kernel Image --ramdisk boot.img-ramdisk.gz --dt_dir j7e3g -o boot-new2.img
mkdir $MYOUT/tmp2
$MYTOOLS/unpackbootimg -i boot-new2.img -o tmp2
cp $MYOUT/tmp2/boot-new2.img-dt $ABDIR/zipsrc/kernel/dt.img
rm -rf $MYOUT/tmp2
rm $MYOUT/boot-new2.img

# copy the kernel
cp Image $ABDIR/zipsrc/kernel/zImage

# cleanup
rm -rf $MYOUT/j7e3g/
rm $MYOUT/Image.gz-dtb
rm $MYOUT/boot.img-ramdisk.gz

# make the flashable zip new ramdisk
cd $ABDIR/zipsrc

zip -r afterburner-N-v.zip kernel/ bootimgtools/ add-ons/ META-INF/

mv afterburner-N-v.zip $ABDIR/out/
rm $ABDIR/zipsrc/kernel/dt.img
rm $ABDIR/zipsrc/kernel/zImage
