#ifndef SLEAK_FRAME_OUTPUT_GLSL
#define SLEAK_FRAME_OUTPUT_GLSL

#include "sleak/frame.glsl"
#include "sleak/tonemap.glsl"

/// This frame's output transform (FrameData version 2).
SleakOutputParams SleakFrameOutput() {
    SleakOutputParams o;
    o.op = uFrame.Flags.z;
    o.exposure = uFrame.Output.x;
    o.gamma = uFrame.Output.y;
    o.saturation = uFrame.Output.z;
    o.dither = uFrame.Output.w;
    o.curve = uFrame.OutputCurve.x;
    o.white = uFrame.WhitePath;
    o.whiteInverse = uFrame.WhitePathInverse;
    return o;
}

#endif
