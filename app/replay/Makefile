#
# Makefile
#

CIRCLEHOME = ../..

OBJS	= main.o kernel.o v3d.o v3d_replay_linux.o 
OBJS += gen/py-videocore6-mininput/mem_csd_0001.o
#OBJS += gen/py-videocore6-mininput/dummy.o

#LIBS	= $(CIRCLEHOME)/app/lib/v3d/v3d.a \
	  $(CIRCLEHOME)/lib/libcircle.a

LIBS	= $(CIRCLEHOME)/lib/libcircle.a
LIBS	+= $(CIRCLEHOME)/app/lib/zlib_inflate/libz_inflate.a
LIBS	+= $(CIRCLEHOME)/addon/linux/liblinuxemu.a  # for printk
	  
include ../Rules.mk

# CFLAGS += -DV3D_LOAD_FROM_FILE

-include $(DEPS)
