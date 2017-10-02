PROGS		= macbootmake
RM		= rm
# for cc65:
SYS		= c128
CLIB = --lib $(SYS).lib
CL   = cl65
CC   = cc65
AS   = ca65
LD   = ld65
%: %.c
%: %.s
.c.o:
	@echo $<
	@$(CC) $(CFLAGS) -Oirs --codesize 500 -T -g -t $(SYS) $<
	@$(AS) $(<:.c=.s)
.s.o:
	@echo $<
	@$(AS) $(AFLAGS) -t $(SYS) $<
.o:
	@$(LD) -o $@ -t $(SYS) -m $@.map $^ $(CLIB)

all: $(PROGS)

macbootmake: macbootmake.o

clean:
	-$(RM) -f *.o *.tmp $(PROGS) *~ _*.tmp* core
