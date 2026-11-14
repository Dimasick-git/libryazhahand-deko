// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Dimasick-git (RCU)
#pragma once
#include <switch.h>

// Код шейдеров стекла (dksh) — готовыми массивами, см. libglass/tools/build_shaders.py.
extern const u8 glass_present_vert_dksh[];
extern const u32 glass_present_vert_dksh_size;
extern const u8 glass_present_frag_dksh[];
extern const u32 glass_present_frag_dksh_size;
extern const u8 glass_capture_frag_dksh[];
extern const u32 glass_capture_frag_dksh_size;
extern const u8 glass_blur_down_frag_dksh[];
extern const u32 glass_blur_down_frag_dksh_size;
extern const u8 glass_blur_up_frag_dksh[];
extern const u32 glass_blur_up_frag_dksh_size;
