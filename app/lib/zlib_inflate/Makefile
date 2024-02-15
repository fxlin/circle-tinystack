#
# Makefile
#

CIRCLEHOME = ../../..

OBJS	= inffast.o inflate.o infutil.o \
		     inftrees.o 
		     
# inflate_syms.o

libz_inflate.a: $(OBJS)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) cr $@ $(OBJS)

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
