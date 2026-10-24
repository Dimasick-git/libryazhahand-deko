#---------------------------------------------------------------------------------
# Ryazhahand library configuration
# Auto-detects libryazhahand directory location.
#
# Подключается в Makefile оверлея одной строкой:
#   include ${TOPDIR}/lib/libryazhahand/ryazhahand.mk
#
# Стекло (живой размытый кадр игры под интерфейсом, вывод через видеочип) — строкой ДО include:
#   USING_GLASS_DIRECTIVE := 1
# Больше ничего не нужно: шейдеры лежат готовыми, deko3d собирается вместе с оверлеем (libglass/README.md).
#---------------------------------------------------------------------------------

TOPDIR ?= $(CURDIR)

# Absolute path of this .mk file directory.
RYZ_ABS := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# Relative path from TOPDIR (fallback to absolute if .mk lies outside TOPDIR).
RYZ_DIR := $(subst $(TOPDIR)/,,$(RYZ_ABS))
ifeq ($(RYZ_DIR),$(RYZ_ABS))
  RYZ_DIR := $(RYZ_ABS)
endif

SOURCES += \
  $(RYZ_DIR)/common \
  $(RYZ_DIR)/libryazha/source \
  $(RYZ_DIR)/libtesla/source

INCLUDES += \
  $(RYZ_DIR)/common \
  $(RYZ_DIR)/libryazha/include \
  $(RYZ_DIR)/libtesla/include

ifeq ($(strip $(USING_GLASS_DIRECTIVE)),1)
SOURCES += \
  $(RYZ_DIR)/libglass/source \
  $(RYZ_DIR)/libglass/deko3d/source \
  $(RYZ_DIR)/libglass/deko3d/source/maxwell

INCLUDES += \
  $(RYZ_DIR)/libglass/deko3d/include \
  $(RYZ_DIR)/libglass/deko3d/generated

# deko3d видит свои типы внутренними, стекло — непрозрачными (deko3d.h, __DK_INTERNAL__): по ABI они одинаковы, но
# LTO на весь оверлей сшил бы два разных определения одного типа. Как у upstream (отдельная библиотека), файлы deko3d
# собираются без LTO и, как у upstream, без исключений и RTTI (оверлей с исключениями иначе ругается на noexcept).
# -Wmaybe-uninitialized в dk_device.cpp — ложное на -Os (upstream собирает -O2 -Werror).
RYZ_GLASS_DEKO3D_OBJS := $(patsubst %.cpp,%.o,$(notdir $(wildcard \
  $(RYZ_ABS)/libglass/deko3d/source/*.cpp $(RYZ_ABS)/libglass/deko3d/source/maxwell/*.cpp)))
$(RYZ_GLASS_DEKO3D_OBJS): CXXFLAGS += -fno-lto -fno-exceptions -fno-rtti -Wno-maybe-uninitialized
endif
