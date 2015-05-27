# tw6869

tw6869 driver posted by Alexander Kudjashev 
in https://community.freescale.com/message/518226#518226

this is "work in progress"

## bitbake recipe to build the kernel module with yocto

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
```
