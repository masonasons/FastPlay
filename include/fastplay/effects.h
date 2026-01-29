#pragma once
#ifndef FASTPLAY_EFFECTS_H
#define FASTPLAY_EFFECTS_H

#include "types.h"
#include "bass.h"

// Effect initialization
bool InitEffects();
void FreeEffects();

// Stream effect management (Volume, Pitch, Tempo, Rate)
void ToggleStreamEffect(int effectIndex);
bool IsStreamEffectEnabled(int effectIndex);

// DSP effect management
void ToggleDSPEffect(DSPEffectType type);
void EnableDSPEffect(DSPEffectType type, bool enable);
bool IsDSPEffectEnabled(DSPEffectType type);
void ApplyDSPEffects();  // Call after stream creation
void RemoveDSPEffects(); // Call before stream destruction

// Reverb algorithm selection (0=Off, 1=Freeverb, 2=DX8, 3=I3DL2)
void SetReverbAlgorithm(int algorithm);

// Parameter getters
float GetParamValue(ParamId id);
const char* GetParamName(ParamId id);
const char* GetParamUnit(ParamId id);
const ParamDef* GetParamDef(ParamId id);

// Parameter setters
void SetParamValue(ParamId id, float value);

// Parameter cycling (builds list from enabled effects)
void CycleParam(int direction);
void AdjustCurrentParam(int direction);
void ResetCurrentParam();
void SetCurrentParamToMin();
void SetCurrentParamToMax();
void AnnounceCurrentParam();
int GetAvailableParamCount();

// Reset all effects to default
void ResetEffects();

// Legacy compatibility
float GetEffectValue(EffectType type);
const char* GetEffectName(EffectType type);
const char* GetEffectUnit(EffectType type);
void SetEffectValue(EffectType type, float value);
void CycleEffect(int direction);
void AdjustCurrentEffect(int direction);

#endif // FASTPLAY_EFFECTS_H
