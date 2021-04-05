#
# Makefile
#

CIRCLEHOME = ../..

OBJS	= main.o kernel.o v3d.o

#LIBS	= $(CIRCLEHOME)/app/lib/v3d/v3d.a \
	  $(CIRCLEHOME)/lib/libcircle.a

LIBS	= $(CIRCLEHOME)/lib/libcircle.a
LIBS	+= $(CIRCLEHOME)/addon/linux/liblinuxemu.a  # for printk
	  
include ../Rules.mk

-include $(DEPS)
