#!/bin/bash

# Get the directory of the script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHADERS_DIR="$SCRIPT_DIR/../bejzak_engine/shaders"
ASSETS_DIR="$SCRIPT_DIR/../assets/shaders"

# Compile shaders with glslc
glslc -fshader-stage=vertex      "$SHADERS_DIR/shader_blinn_phong.vert.glsl"      -O -o "$ASSETS_DIR/shader_blinn_phong.vert.spv"
glslc -fshader-stage=tesscontrol "$SHADERS_DIR/shader_blinn_phong.tsc.glsl"       -O -o "$ASSETS_DIR/shader_blinn_phong.tsc.spv"
glslc -fshader-stage=tesseval    "$SHADERS_DIR/shader_blinn_phong.tse.glsl"       -O -o "$ASSETS_DIR/shader_blinn_phong.tse.spv"
glslc -fshader-stage=fragment    "$SHADERS_DIR/shader_blinn_phong.frag.glsl"      -O -o "$ASSETS_DIR/shader_blinn_phong.frag.spv"

glslc -fshader-stage=fragment    "$SHADERS_DIR/offscreen.frag.glsl"               -O -o "$ASSETS_DIR/off.frag.spv"

glslc -fshader-stage=vertex      "$SHADERS_DIR/skybox.vert.glsl"                  -O -o "$ASSETS_DIR/skybox.vert.spv"
glslc -I "$SHADERS_DIR/bindless.glsl" -fshader-stage=fragment "$SHADERS_DIR/skybox.frag.glsl" -O -o "$ASSETS_DIR/skybox.frag.spv"
glslc -fshader-stage=fragment    "$SHADERS_DIR/skybox_offscreen.frag.glsl"        -O -o "$ASSETS_DIR/skybox_offscreen.frag.spv"

glslc -fshader-stage=vertex      "$SHADERS_DIR/shadow.vert.glsl"                  -O -o "$ASSETS_DIR/shadow.vert.spv"
glslc -fshader-stage=fragment    "$SHADERS_DIR/shadow.frag.glsl"                  -O -o "$ASSETS_DIR/shadow.frag.spv"

glslc -I "$SHADERS_DIR/bindless.glsl" -fshader-stage=vertex "$SHADERS_DIR/shader_pbr.vert.glsl" -O -o "$ASSETS_DIR/shader_pbr.vert.spv"

glslc -fshader-stage=vertex      "$SHADERS_DIR/shader_pbr_tesselation.vert.glsl"  -O -o "$ASSETS_DIR/shader_pbr_tesselation.vert.spv"
glslc -fshader-stage=tesscontrol "$SHADERS_DIR/shader_pbr_tesselation.tsc.glsl"   -O -o "$ASSETS_DIR/shader_pbr_tesselation.tsc.spv"
glslc -fshader-stage=tesseval    "$SHADERS_DIR/shader_pbr_tesselation.tse.glsl"   -O -o "$ASSETS_DIR/shader_pbr_tesselation.tse.spv"
glslc -fshader-stage=fragment    "$SHADERS_DIR/shader_pbr_tesselation.frag.glsl"  -O -o "$ASSETS_DIR/shader_pbr_tesselation.frag.spv"

glslc -I "$SHADERS_DIR/bindless.glsl" -fshader-stage=fragment "$SHADERS_DIR/shader_pbr.frag.glsl" -O -o "$ASSETS_DIR/shader_pbr.frag.spv"

glslc -fshader-stage=fragment    "$SHADERS_DIR/offscreen_shader_pbr.frag.glsl"    -O -o "$ASSETS_DIR/offscreen_shader_pbr.frag.spv"
