# MicroPython user C module `samara` (desktop windows, syscall 500).
# Built via: make -C ports/unix USER_C_MODULES=<SamaraOS>/userland/samara/micropython
SAMARA_MOD_DIR := $(USERMOD_DIR)
SRC_USERMOD += $(SAMARA_MOD_DIR)/samara.c
