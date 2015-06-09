# tw6869

tw6869 driver posted by Alexander Kudjashev 
in https://community.freescale.com/message/518226#518226

this is "work in progress"

################ build the driver in-tree:

- cd drivers/media/pci

- git clone git@github.com:FrankBau/tw6869.git

- add a line to drivers/media/pci/Makefile

    obj-$(CONFIG_VIDEO_TW6869) += tw6869/

- add a line to drivers/media/pci/Kconfig in the block "if MEDIA_ANALOG_TV_SUPPORT"

    source "drivers/media/pci/tw6869/Kconfig"

Then, enable the driver in menuconfig and re-build the kernel modules:

    make menuconfig

	 Symbol: VIDEO_TW6869 [=m]
	 Type  : tristate
	 Prompt: TW6869
	   Location:
	     -> Device Drivers
	       -> Multimedia support (MEDIA_SUPPORT [=y])
		 -> Media PCI Adapters (MEDIA_PCI_SUPPORT [=y])
	   Defined at drivers/media/pci/tw6869/Kconfig:1
	   Depends on: MEDIA_SUPPORT [=y] && MEDIA_PCI_SUPPORT [=y] && MEDIA_ANALOG_TV_SUPPORT [=y] && PCI [=y]
	   Selects: VIDEOBUF2_DMA_CONTIG [=m]

    make modules

###### bitbake recipe to build the kernel module out-of-tree with yocto

```
# tw6869 driver posted by Alexander Kudjashev in https://community.freescale.com/message/518226#518226

# needs kernel CONFIG: VIDEOBUF2_DMA_CONTIG

inherit module

PR = "r1"
PV = "1.2.0"

SRC_URI = "git://github.com/FrankBau/tw6869.git"
SRCREV = "798bd2a12994da723eb5ceb4f2051db3f1815b71"

SUMMARY = "tw6869 Linux kernel module"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=2c1c00f9d3ed9e24fa69b932b7e7aff2"

S = "${WORKDIR}/git"

###### end of bitbake recipe 
