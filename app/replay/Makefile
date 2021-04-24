#
# Makefile
#

CIRCLEHOME = ../..

OBJS	= main.o kernel.o v3d.o v3d_replay_linux.o gen/py-videocore6-mininput/mem_csd_0001.o

#LIBS	= $(CIRCLEHOME)/app/lib/v3d/v3d.a \
	  $(CIRCLEHOME)/lib/libcircle.a

LIBS	= $(CIRCLEHOME)/lib/libcircle.a
LIBS	+= $(CIRCLEHOME)/addon/linux/liblinuxemu.a  # for printk
	  
include ../Rules.mk

# CFLAGS += -DV3D_LOAD_FROM_FILE

-include $(DEPS)
