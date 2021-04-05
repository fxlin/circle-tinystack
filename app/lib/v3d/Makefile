#
# Makefile
#

CIRCLEHOME = ../../..

OBJS	= v3d.o

libv3d.a: $(OBJS)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) cr $@ $(OBJS)

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
