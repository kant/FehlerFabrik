// #include "plugin.hpp"
#include "wavetables/Wavetables.hpp"

struct DCBlock
{
    // https://www.dsprelated.com/freebooks/filters/DC_Blocker.html

    float xm1 = 0;
    float ym1 = 0;

    float r = 0.995;

    float process(float x)
    {
        float y = x - xm1 + r * ym1;
        xm1 = x;
        ym1 = y;
        return y;
    }
};

// Ramp generator based on Befaco Rampage
// https://github.com/VCVRack/Befaco/blob/v1/src/Rampage.cpp
struct Ramp
{
    float minTime = 1e-3;
    float shape = 0.0;
    float out = 0.f;
    bool gate = false;

    float shapeDelta(float delta, float tau, float shape)
    {
        float lin = sgn(delta) * 10.f / tau;
        if (shape < 0.f)
        {
            float log = sgn(delta) * 40.f / tau / (fabs(delta) + 1.f);
            return crossfade(lin, log, -shape * 0.95f);
        }
        else
        {
            float exp = M_E * delta / tau;
            return crossfade(lin, exp, shape * 0.90f);
        }
    }

    dsp::PulseGenerator endOfCyclePulse;

    void process(float shape, float riseRate, float fallRate, float time, bool cycle)
    {
        float in = 0;
        if (gate)
        {
            in = 1.f;
        }

        float delta = in - out;

        bool rising = false;
        bool falling = false;

        if (delta > 0)
        {
            // Rise removed for now, just decay
            // Rise
            float riseCv = riseRate;
            float rise = minTime * pow(2.0, riseCv * 20.0);
            out += shapeDelta(delta, rise, shape) * time;
            rising = (in - out > 1e-3);
            if (!rising)
            {
                gate = false;
            }
        }
        else if (delta < 0)
        {
            // Fall
            // Just control knob for now, will add CV control later
            float fallCv = fallRate;
            fallCv = clamp(fallCv, 0.0f, 1.0f);
            float fall = minTime * pow(2.0f, fallCv * 20.0f);
            out += shapeDelta(delta, fall, shape) * time;
            falling = (in - out < -1e-3);
            if (!falling)
            {
                // End of cycle, check if we should turn the gate back on (cycle mode)
                endOfCyclePulse.trigger(1e-3);
                if (cycle)
                {
                    gate = true;
                }
            }
        }
        else
        {
            gate = false;
        }

        if (!rising && !falling)
        {
            out = in;
        }
    }
};

// Wavetable operator for FM synthesis
struct Operator
{
    float phase = 0.f;
    float freq = 0.f;
    float wave = 0.f;
    float out = 0.f;
    float bufferSample1 = 0.f;
    float bufferSample2 = 0.f;
    float feedbackSample = 0.f;

    void setPitch(float pitch)
    {
        // The default pitch is C4 = 256.6256f
        freq = dsp::FREQ_C4 * pow(2.f, pitch);
    }

    void applyRatio(float ratio)
    {
        freq *= ratio;
    }

    void process(float time, float amplitude, float fmMod, float feedback, int table)
    {
        phase += freq * time + fmMod * 0.5f;
        if (phase >= 0.5f)
        {
            phase -= 1.f;
        }
        else if (phase <= -0.5f)
        {
            phase += 1.f;
        }

        float wtPOS = (phase + feedback * feedbackSample);
        // Wrap wavetable position between 0.f and 1.f
        wtPOS = eucMod(wtPOS, 1.f);

        float *waveTable = wavetable_opal[table];
        float tableLength = wavetable_opal_lengths[table];

        wtPOS *= (tableLength);
        wave = interpolateLinear(waveTable, wtPOS);

        out = wave * amplitude;

        bufferSample1 = wave;
        bufferSample2 = bufferSample1;
        feedbackSample = (bufferSample1 + bufferSample2) / 2.f;
    }
};

// 3D modulation matrix
// 6 algorithims, 4 sources (each operators sine output), 5 destinations (each ops fm in and the master output)
float modMatrix[6][4][5] =
    {
        {{0, 1, 0, 0, 0}, {0, 0, 0, 0, 1}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},
        {{0, 0, 0, 0, 0.5}, {0, 0, 0, 0, 0.5}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},
        {{0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, 1, 0}, {0, 0, 0, 0, 1}},
        {{0, 1, 0, 0, 0}, {0, 0, 0, 0, 0.5}, {0, 0, 0, 1, 0}, {0, 0, 0, 0, 0.5}},
        {{0, 0, 0, 0, 0.5}, {0, 0, 1, 0, 0}, {0, 0, 0, 1, 0}, {0, 0, 0, 0, 0.5}},
        {{0, 0, 0, 0, 0.3}, {0, 0, 1, 0, 0}, {0, 0, 0, 0, 0.3}, {0, 0, 0, 0, 0.3}},
};

// 23 Frequency ratios taken from Mutable Instruments Plaits 2 OP FM mode
// https://github.com/pichenettes/eurorack/blob/master/plaits/resources/lookup_tables.py
float fm_frequency_ratios[23] = {0.5f, 0.5f * pow(2.f, (16.f / 1200.0f)),
                                 sqrt(2.f) / 2.f, M_PI / 4.f, 1.0f, 1.0f * pow(2.f, (16.f / 1200.0f)), sqrt(2.f),
                                 M_PI / 2.f, 7.0f / 4.f, 2.f, 2.f * pow(2.f, (16.f / 1200.0f)), 9.0f / 4.f, 11.0f / 4.f,
                                 2.f * sqrt(2.f), 3.f, M_PI, sqrt(3.f) * 2.f, 4.f, sqrt(2.f) * 3.f,
                                 M_PI * 3.f / 2.f, 5.f, sqrt(2.f) * 4.f, 8.f};

// 32 Combinations of the above ratios that sound interesting.
int ratioMatrix[32][4] = {
    {5, 5, 5, 5}, {3, 5, 7, 5}, {0, 5, 8, 5}, {7, 5, 2, 5}, {9, 5, 10, 5}, {14, 5, 15, 5}, {14, 8, 9, 5}, {14, 11, 8, 5}, {9, 8, 12, 5}, {22, 14, 17, 4}, {14, 14, 12, 4}, {9, 11, 14, 7}, {22, 9, 9, 13}, {15, 8, 17, 13}, {10, 12, 6, 15}, {10, 12, 6, 16}, {17, 12, 6, 11}, {5, 14, 8, 12}, {5, 10, 13, 12}, {5, 14, 14, 14}, {4, 2, 5, 2}, {0, 8, 16, 9}, {3, 13, 14, 1}, {4, 12, 14, 1}, {0, 10, 9, 0}, {0, 14, 13, 13}, {0, 10, 4, 16}, {0, 3, 4, 18}, {0, 1, 4, 13}, {14, 0, 12, 22}, {15, 0, 5, 22}, {1, 14, 9, 4}};

// 64 combinations of waveform
int tableMatrix[64][4] = {
    {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 2, 0, 2}, {0, 3, 0, 3}, {0, 4, 0, 4}, {0, 5, 0, 5}, {0, 6, 0, 6}, {0, 7, 0, 7}, {1, 0, 1, 0}, {1, 1, 1, 1}, {1, 2, 1, 2}, {1, 3, 1, 3}, {1, 4, 1, 4}, {1, 5, 1, 5}, {1, 6, 1, 6}, {1, 7, 1, 7}, {2, 0, 2, 0}, {2, 1, 2, 1}, {2, 2, 2, 2}, {2, 3, 2, 3}, {2, 4, 2, 4}, {2, 5, 2, 5}, {2, 6, 2, 6}, {2, 7, 2, 7}, {3, 0, 3, 0}, {3, 1, 3, 1}, {3, 2, 3, 2}, {3, 3, 3, 3}, {3, 4, 3, 4}, {3, 5, 3, 5}, {3, 6, 3, 6}, {3, 7, 3, 7}, {4, 0, 4, 0}, {4, 1, 4, 1}, {4, 2, 4, 2}, {4, 3, 4, 3}, {4, 4, 4, 4}, {4, 5, 4, 5}, {4, 6, 4, 6}, {4, 7, 4, 7}, {5, 0, 5, 0}, {5, 1, 5, 1}, {5, 2, 5, 2}, {5, 3, 5, 3}, {5, 4, 5, 4}, {5, 5, 5, 5}, {5, 6, 5, 6}, {5, 7, 5, 7}, {6, 0, 6, 0}, {6, 1, 6, 1}, {6, 2, 6, 2}, {6, 3, 6, 3}, {6, 4, 6, 4}, {6, 5, 6, 5}, {6, 6, 6, 6}, {6, 7, 6, 7}, {7, 0, 7, 0}, {7, 1, 7, 1}, {7, 2, 7, 2}, {7, 3, 7, 3}, {7, 4, 7, 4}, {7, 5, 7, 5}, {7, 6, 7, 6}, {7, 7, 7, 7}};