# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_CL_DSP)		+= cl_dsp-core.o
cl_dsp-core-y			:= cl_dsp.o
cl_dsp-core-$(CONFIG_DEBUG_FS)	+= cl_dsp-debugfs.o
obj-$(CONFIG_INPUT_CS40L26)	+= cs40l26-core.o cs40l26-i2c.o
cs40l26-core-y			:= cs40l26.o cs40l26-tables.o cs40l26-sysfs.o
cs40l26-core-$(CONFIG_DEBUG_FS)	+= cs40l26-debugfs.o
