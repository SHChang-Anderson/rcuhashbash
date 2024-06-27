TARGET_MODULE := rcuhashbash-resize
TARGET_MODULE2 := rcuhashbash-resize_wob_lock

obj-m += $(TARGET_MODULE).o
obj-m += $(TARGET_MODULE2).o

GIT_HOOKS := .git/hooks/applied

all: $(GIT_HOOKS) $(TARGET_MODULE).c $(TARGET_MODULE2).c
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo
	
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

load:
	sudo insmod $(TARGET_MODULE).ko
	sudo insmod $(TARGET_MODULE2).ko

unload:
	sudo rmmod $(TARGET_MODULE) || true >/dev/null
	sudo rmmod $(TARGET_MODULE2) || true >/dev/null