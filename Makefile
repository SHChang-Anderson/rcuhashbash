TARGET_MODULE := rcuhashbash-resize
# obj-m += rcuhashbash.o
obj-m += $(TARGET_MODULE).o
GIT_HOOKS := .git/hooks/applied
all: $(GIT_HOOKS) rcuhashbash-resize.c
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo
	
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

load:
	sudo insmod $(TARGET_MODULE).ko
unload:
	sudo rmmod $(TARGET_MODULE) || true >/dev/null