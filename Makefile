TARGET = BlockBlast
OBJS = main.o

CFLAGS = -O2 -G0 -Wall -std=gnu99
LIBS = -lpspdebug -lpspaudio -lpspdisplay -lpspge -lpspctrl -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Block Blast
PSP_EBOOT_ICON = ICON0.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
