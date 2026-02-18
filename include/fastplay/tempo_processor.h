#pragma once
#ifndef FASTPLAY_TEMPO_PROCESSOR_H
#define FASTPLAY_TEMPO_PROCESSOR_H

#include <windows.h>
#include "bass.h"

// Tempo/pitch algorithm types
enum class TempoAlgorithm {
    SoundTouch,     // BASS_FX (SoundTouch-based) - fast, good for speech
    RubberBandR2,   // Rubber Band R2 (Faster) engine - balanced
    RubberBandR3,   // Rubber Band R3 (Finer) engine - highest quality
    Speedy,         // Google Speedy - nonlinear speech speedup
    Signalsmith,    // Signalsmith Stretch - high quality pitch/time
    COUNT
};

// Algorithm names for UI
const char* GetAlgorithmName(TempoAlgorithm algo);
const char* GetAlgorithmDescription(TempoAlgorithm algo);

// Abstract tempo processor interface
class TempoProcessor {
public:
    virtual ~TempoProcessor() = default;

    // Initialize the processor for a given source stream
    // Returns the playback stream (may be same as source or a wrapper)
    virtual HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) = 0;

    // Clean up resources
    virtual void Shutdown() = 0;

    // Set tempo change in percentage (-50 to +100)
    virtual void SetTempo(float tempoPercent) = 0;

    // Set pitch change in semitones (-12 to +12)
    virtual void SetPitch(float semitones) = 0;

    // Set rate multiplier (0.5 to 2.0) - affects both tempo and pitch
    virtual void SetRate(float rate) = 0;

    // Get current values
    virtual float GetTempo() const = 0;
    virtual float GetPitch() const = 0;
    virtual float GetRate() const = 0;

    // Check if processor is active
    virtual bool IsActive() const = 0;

    // Get the algorithm type
    virtual TempoAlgorithm GetAlgorithm() const = 0;

    // Position and length (in seconds) - needed for Rubber Band
    virtual double GetLength() const = 0;
    virtual double GetPosition() const = 0;
    virtual void SetPosition(double seconds) = 0;

    // Get the source stream (for seeking with Rubber Band)
    virtual HSTREAM GetSourceStream() const = 0;
};

// Factory function to create a tempo processor
TempoProcessor* CreateTempoProcessor(TempoAlgorithm algorithm);

// Get/set the global algorithm preference
TempoAlgorithm GetCurrentAlgorithm();
void SetCurrentAlgorithm(TempoAlgorithm algorithm);

// Global tempo processor instance management
void InitTempoProcessor();
void FreeTempoProcessor();
TempoProcessor* GetTempoProcessor();

#endif // FASTPLAY_TEMPO_PROCESSOR_H
