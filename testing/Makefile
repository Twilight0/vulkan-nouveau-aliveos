CC ?= gcc
MINGW_CC ?= x86_64-w64-mingw32-gcc
MESA_INC := $(wildcard ../src/mesa-*/include)
CFLAGS ?= -O2 -Wall -Wextra -Werror $(addprefix -I,$(MESA_INC))
LDFLAGS ?= -lvulkan

SPV_HEADERS = \
	shaders/tri.vert.spv.h \
	shaders/tri.frag.spv.h \
	shaders/tex.frag.spv.h \
	shaders/comp_ssbo.comp.spv.h \
	shaders/comp_tex_dyn.comp.spv.h \
	shaders/comp_img_store.comp.spv.h \
	shaders/comp_img_rw.comp.spv.h \
	shaders/indexed_ubo.vert.spv.h \
	shaders/indexed_ubo.frag.spv.h

all: vk_smoke

shaders/%.vert.spv.h: shaders/%.vert
	glslangValidator -V --vn $*_vert_spv -o $@ $<

shaders/%.frag.spv.h: shaders/%.frag
	glslangValidator -V --vn $*_frag_spv -o $@ $<

shaders/%.comp.spv.h: shaders/%.comp
	glslangValidator -V --vn $*_comp_spv -o $@ $<

vk_smoke: vk_smoke.c $(SPV_HEADERS)
	$(CC) $(CFLAGS) -I. -o $@ $< $(LDFLAGS)

d3d11_smoke.exe: d3d11_smoke.c
	$(MINGW_CC) -O2 -Wall $< -o $@ -ld3d11 -ldxgi -luuid

clean:
	rm -f vk_smoke d3d11_smoke.exe shaders/*.spv.h

.PHONY: all clean
