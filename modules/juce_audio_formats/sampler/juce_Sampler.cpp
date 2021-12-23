/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 7 End-User License
   Agreement and JUCE Privacy Policy.

   End User License Agreement: www.juce.com/juce-7-licence
   Privacy Policy: www.juce.com/juce-privacy-policy

   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

SamplerSound::SamplerSound (const String& soundName,
                            AudioFormatReader& source,
                            const BigInteger& notes,
                            int midiNoteForNormalPitch,
                            double attackTimeSecs,
                            double releaseTimeSecs,
                            double maxSampleLengthSeconds)
    : name (soundName),
      sourceSampleRate (source.sampleRate),
      midiNotes (notes),
      midiRootNote (midiNoteForNormalPitch)
{
    if (sourceSampleRate > 0 && source.lengthInSamples > 0)
    {
        length = jmin ((int) source.lengthInSamples,
                       (int) (maxSampleLengthSeconds * sourceSampleRate));

        data.reset (new AudioBuffer<float> (jmin (2, (int) source.numChannels), length + 4));

        source.read (data.get(), 0, length + 4, 0, true, true);

        params.attack  = static_cast<float> (attackTimeSecs);
        params.release = static_cast<float> (releaseTimeSecs);
    }
}

SamplerSound::~SamplerSound()
{
}

bool SamplerSound::appliesToNote (int midiNoteNumber)
{
    return midiNotes[midiNoteNumber];
}

bool SamplerSound::appliesToChannel (int /*midiChannel*/)
{
    return true;
}

//==============================================================================
SamplerVoice::SamplerVoice(int32_t iBufferSize) : bufferSize(iBufferSize)
{
    fadeBuffer.reset (new AudioBuffer<float> (2, bufferSize));
}

SamplerVoice::~SamplerVoice() {}

bool SamplerVoice::canPlaySound (SynthesiserSound* sound)
{
    return dynamic_cast<const SamplerSound*> (sound) != nullptr;
}

#undef DBG
#define DBG(textToWrite)

void SamplerVoice::startNote (int midiNoteNumber, float velocity, SynthesiserSound* s, int /*currentPitchWheelPosition*/)
{
    if (isFading)
        DBG (reinterpret_cast<uint64_t>(this) << " SamplerVoice::startNote isFading = true");
    else
        DBG (reinterpret_cast<uint64_t>(this) << " SamplerVoice::startNote isFading = false");

    if (auto* sound = dynamic_cast<const SamplerSound*> (s))
    {
        pitchRatio = std::pow (2.0, (midiNoteNumber - sound->midiRootNote) / 12.0)
                        * sound->sourceSampleRate / getSampleRate();

        sourceSamplePosition = 0.0;
        lgain = velocity;
        rgain = velocity;

        adsr.setSampleRate (sound->sourceSampleRate);
        adsr.setParameters (sound->params);

        adsr.noteOn();
    }
    else
    {
        jassertfalse; // this object can only play SamplerSounds!
    }
}

void SamplerVoice::stopNote (float /*velocity*/, bool allowTailOff)
{
    if (allowTailOff)
    {
        DBG (reinterpret_cast<uint64_t>(this) << " SamplerVoice::stopNote - tail off true");
        adsr.noteOff();
    }
    else
    {
        DBG (reinterpret_cast<uint64_t>(this) << " SamplerVoice::stopNote - tail off false");

#if 1
        if (renderingFade)
        {
            DBG (reinterpret_cast<uint64_t>(this) << " SamplerVoice::stopNote - currently rendering a fade");
            return;
        }
        
        fadeBuffer->clear();
        renderingFade = true;
        renderNextBlock(*fadeBuffer.get(), 0, bufferSize);
        renderingFade = false;

        // If the pitchRatio is less than 1, the lenght of the ramp
        // needs to be computed since it will not use the entire buffer
        // to get the 0
        //
        int32_t endSample = bufferSize;
        if (pitchRatio < 1.0)
        {
            endSample = static_cast<int32_t>(endSample * pitchRatio);
        }
        
        fadeBuffer->applyGainRamp(0, 0, endSample, startingGain, 0);
        
        if (fadeBuffer->getNumChannels() > 1)
            fadeBuffer->applyGainRamp(1, 0, endSample, startingGain, 0);
        
        // Need to clear the remaining samples after ramping to 0
        //
        fadeBuffer->clear(endSample, (bufferSize - endSample));
        
        isFading = true;
#endif

        isFading = true;
        clearCurrentNote();
        adsr.reset();
    }
}

void SamplerVoice::pitchWheelMoved (int /*newValue*/) {}
void SamplerVoice::controllerMoved (int /*controllerNumber*/, int /*newValue*/) {}

//==============================================================================
void SamplerVoice::renderNextBlock (AudioBuffer<float>& outputBuffer, int startSample, int numSamples)
{
    if (isFading)
    {
        DBG (reinterpret_cast<uint64_t>(this) << " SamplerVoice::renderNextBlock - fading - startSample = " << startSample << " numSamples = " << numSamples << " sourceSamplePosition = " << sourceSamplePosition);
        isFading = false;

        auto numChannels = outputBuffer.getNumChannels();
        for (auto channel = 0; channel != numChannels; ++channel)
        {
            if (fadeBuffer->getNumChannels() > channel)
            {
                outputBuffer.copyFrom (channel, 0, fadeBuffer->getReadPointer (channel), numSamples);
            }
        }
    }
    else
    if (auto* playingSound = static_cast<SamplerSound*> (getCurrentlyPlayingSound().get()))
    {
        DBG (reinterpret_cast<uint64_t>(this) << " SamplerVoice::renderNextBlock - playing sound - startSample = " << startSample << " numSamples = " << numSamples << " sourceSamplePosition = " << sourceSamplePosition);
        
        auto& data = *playingSound->data;
        const float* const inL = data.getReadPointer (0);
        const float* const inR = data.getNumChannels() > 1 ? data.getReadPointer (1) : nullptr;

        float* outL = outputBuffer.getWritePointer (0, startSample);
        float* outR = outputBuffer.getNumChannels() > 1 ? outputBuffer.getWritePointer (1, startSample) : nullptr;

        bool captureStartingGain = true;

        while (--numSamples >= 0)
        {
            auto pos = (int) sourceSamplePosition;
            auto alpha = (float) (sourceSamplePosition - pos);
            auto invAlpha = 1.0f - alpha;

            // just using a very simple linear interpolation here..
            float l = (inL[pos] * invAlpha + inL[pos + 1] * alpha);
            float r = (inR != nullptr) ? (inR[pos] * invAlpha + inR[pos + 1] * alpha)
                                       : l;
            auto envelopeValue = adsr.getNextSample();

            if (captureStartingGain)
            {
                captureStartingGain = false;
                startingGain = envelopeValue;
            }

            l *= lgain * envelopeValue;
            r *= rgain * envelopeValue;

            if (outR != nullptr)
            {
                *outL++ += l;
                *outR++ += r;
            }
            else
            {
                *outL++ += (l + r) * 0.5f;
            }

            sourceSamplePosition += pitchRatio;

            if (sourceSamplePosition > playingSound->length)
            {
                stopNote (0.0f, false);
                break;
            }
        }
    }
    else
    {
        DBG (reinterpret_cast<uint64_t>(this) << " SamplerVoice::renderNextBlock - no sound to play - startSample = " << startSample << " numSamples = " << numSamples << " sourceSamplePosition = " << sourceSamplePosition);
    }
}

} // namespace juce
