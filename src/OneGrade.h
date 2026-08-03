// OneGrade — cross-platform OpenFX color grade plugin for DaVinci Resolve.
// Copyright (C) 2026 Matthew Grdinic
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ofxsImageEffect.h"

class OneGradeFactory : public OFX::PluginFactoryHelper<OneGradeFactory>
{
public:
    OneGradeFactory();
    virtual void load() {}
    virtual void unload() {}
    virtual void describe(OFX::ImageEffectDescriptor& p_Desc);
    virtual void describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum p_Context);
    virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum p_Context);
};
