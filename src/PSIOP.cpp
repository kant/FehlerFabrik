#include "plugin.hpp"
#include "PSIOP.hpp"
#include "common.hpp"

struct PSIOP : Module
{
    enum ParamIds
    {
        START_PARAM,
        FINE_PARAM,
        END_PARAM,
        RATIO_PARAM,
        WAVE_PARAM,
        ALGO_PARAM,
        FB_PARAM,
        RATE1_PARAM,
        RATE2_PARAM,
        SPEED_PARAM,
        RATE2ATTEN_PARAM,
        WAVEATTEN_PARAM,
        RATIOATTEN_PARAM,
        NUM_PARAMS
    };
    enum InputIds
    {
        START_INPUT,
        END_INPUT,
        RATIO_INPUT,
        WAVE_INPUT,
        ALGO_INPUT,
        FB_INPUT,
        RATE1_INPUT,
        RATE2_INPUT,
        SPEED_INPUT,
        TRIGGER_INPUT,
        ACCENT_INPUT,
        CHOKE_INPUT,
        NUM_INPUTS
    };
    enum OutputIds
    {
        OUT_OUTPUT,
        DEBUG1_OUTPUT,
        DEBUG2_OUTPUT,
        DEBUG3_OUTPUT,
        DEBUG4_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds
    {
        OUT_LIGHT,
        NUM_LIGHTS
    };

    Operator operators[4];
    Ramp ramps[3];

    dsp::SchmittTrigger trigger;
    dsp::SchmittTrigger choke;
    dsp::SchmittTrigger accent;

    float startPitch = 0;
    float endPitch = 0;
    float finePitch = 0;
    float rates[3] = {};
    int algo = 0;
    int ratioIndex = 0;
    float feedback = 0;
    int table = 0;
    float index = 0.6f; // Global modulation index
    float level = 1.0f;

    PSIOP()
    {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(START_PARAM, -4.f, 4.f, 0.f, "Start Freq", "Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(FINE_PARAM, -0.2f, 0.2f, 0.f, "Start Fine Freq");
        configParam(END_PARAM, -4.f, 4.f, 0.f, "End Freq", "Hz", dsp::FREQ_SEMITONE, dsp::FREQ_C4);
        configParam(RATIO_PARAM, 0.f, 31.f, 0.f, "FM Ratios");
        configParam(WAVE_PARAM, 0.f, 63.f, 0.f, "Wave Combination");
        configParam(ALGO_PARAM, 0.f, 5.f, 0.f, "FM Algorithm");
        configParam(FB_PARAM, 0.f, 1.f, 0.f, "OP 1 Feedback");
        configParam(RATE1_PARAM, 0.f, 1.f, 0.5f, "Operator 1 & 3 Release Envelope");
        configParam(RATE2_PARAM, 0.f, 1.f, 0.5f, "Operator 2 & 4 Release Envelope");
        configParam(SPEED_PARAM, 0.f, 1.f, 0.f, "Pitch Envelope Speed");
        configParam(RATE2ATTEN_PARAM, -1.f, 1.f, 0.f, "Rate 2 Attenuverter");
        configParam(WAVEATTEN_PARAM, -1.f, 1.f, 0.f, "Wave Attenuverter");
        configParam(RATIOATTEN_PARAM, -1.f, 1.f, 0.f, "Ratio Attenuverter");
    }

    void process(const ProcessArgs &args) override
    {
        // Look for input on the trigger
        // All parameters are held on trigger input
        if (trigger.process(inputs[TRIGGER_INPUT].getVoltage() / 2.0f))
        {
            // Look for accent trigger
            if (accent.process(inputs[ACCENT_INPUT].getVoltage() / 2.0f))
            {
                index = 1.f;
                level = 1.8f;
            }
            else
            {
                index = 0.6f;
                level = 1.f;
            }
            // Compute the start and end pitches
            startPitch = params[START_PARAM].getValue();
            startPitch += inputs[START_INPUT].getVoltage();
            finePitch = params[FINE_PARAM].getValue();
            startPitch += finePitch;
            startPitch = clamp(startPitch, -4.f, 4.f);

            endPitch = params[END_PARAM].getValue();
            endPitch += inputs[END_INPUT].getVoltage();
            endPitch = clamp(endPitch, -4.f, 4.f);

            // Get the index for the ratio matrix
            ratioIndex = (int)params[RATIO_PARAM].getValue();
            ratioIndex += (int)round(inputs[RATIO_INPUT].getVoltage() * params[RATIOATTEN_PARAM].getValue());
            ratioIndex = clamp(ratioIndex, 0, 31);

            // Get the wavetable index
            table = (int)params[WAVE_PARAM].getValue();
            table += (int)round(inputs[WAVE_INPUT].getVoltage() * params[WAVEATTEN_PARAM].getValue());
            table = clamp(table, 0, 63);

            // Get the algorithim
            algo = (int)params[ALGO_PARAM].getValue();
            algo += (int)round(inputs[ALGO_INPUT].getVoltage());
            algo = clamp(algo, 0, 5);

            // Get the OP1 feedback amount
            feedback = params[FB_PARAM].getValue();
            feedback += 0.2f * inputs[FB_INPUT].getVoltage();
            feedback = clamp(feedback, 0.f, 1.f);

            // Get the rates for the volume and pitch envelopes
            for (int i = 0; i < 3; i++)
            {
                rates[i] = params[RATE1_PARAM + i].getValue();
                // Special case to factor in rate 2 attenuator
                rates[i] += i == 1 ? 0.2 * params[RATE2ATTEN_PARAM].getValue() * inputs[RATE2_INPUT].getVoltage() : 0.2 * inputs[RATE1_INPUT + i].getVoltage();
                rates[i] = clamp(rates[i], 0.f, 1.f);
            }

            // Trigger
            for (int i = 0; i < 3; i++)
            {
                // Set the gate for the ramps to active
                ramps[i].gate = true;
            }
        }

        // Look for Choke trigger
        if (choke.process(inputs[CHOKE_INPUT].getVoltage() / 2.0f))
        {
            for (int i = 0; i < 3; i++)
            {
                // Set the gate for the ramps to off
                ramps[i].gate = false;
                ramps[i].out = 0.f;
            }
        }

        // Process amplitude ramps
        for (int i = 0; i < 2; i++)
        {
            ramps[i].process(0, 0, rates[i], args.sampleTime, false);
        }

        // Compute current pitch as a function of pitchStart, pitchEnd and the pitch speed envelope
        float pitch = startPitch;
        if (rates[2] > 0.2)
        {
            // looping set to true
            ramps[2].process(0.3, 0, 1 - rates[2], args.sampleTime, true);

            // Crossfade from start pitch to end pitch
            float xf = ramps[2].out;
            pitch = crossfade(endPitch, startPitch, xf);
        }

        // Process operators
        float output = 0.f;

        for (int i = 0; i < 4; i++)
        {
            // Set initial pitch for each operator
            operators[i].setPitch(pitch);

            // Actual per operator ratio to be used is taken from the LUT of magic ratios
            float ratio = fm_frequency_ratios[ratioMatrix[ratioIndex][i]];
            operators[i].applyRatio(ratio);

            float fmMod = 0;

            // Determine how much operator i is modulated by other modulators j++
            for (int j = 0; j < 4; j++)
            {
                fmMod += operators[j].out * index * modMatrix[algo][j][i];
            }

            // Accumulate phase, apply FM modulation, apply appropriate amp modulation
            // Feedback is applied for OP1 only
            // Ramp 1 affects OP1 & OP3 VCA, ramp 2 affects OP2 & OP4
            if (i == 0)
            {
                operators[i].process(args.sampleTime, ramps[0].out, fmMod, feedback, tableMatrix[table][i]);
            }
            else if (i == 2)
            {
                operators[i].process(args.sampleTime, ramps[0].out, fmMod, 0, tableMatrix[table][i]);
            }
            else
            {
                operators[i].process(args.sampleTime, ramps[1].out, fmMod, 0, tableMatrix[table][i]);
            }

            // Send to output as dependent on Algorithim
            output += operators[i].out * modMatrix[algo][i][4];
            outputs[DEBUG1_OUTPUT + i].setVoltage(operators[i].out);
        }

        // Send output signal to output jack
        outputs[OUT_OUTPUT].setVoltage(output * 4 * level);
    }
};

struct PSIOPWidget : ModuleWidget
{
    PSIOPWidget(PSIOP *module)
    {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/PSIOP.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(22.906, 22.493)), module, PSIOP::START_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(34.099, 35.906)), module, PSIOP::FINE_PARAM));
        addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(52.155, 35.906)), module, PSIOP::END_PARAM));
        addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(72.824, 35.906)), module, PSIOP::RATIO_PARAM));
        addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(60.124, 55.132)), module, PSIOP::WAVE_PARAM));
        addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(20.493, 74.418)), module, PSIOP::ALGO_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(81.273, 65.867)), module, PSIOP::FB_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(33.457, 55.132)), module, PSIOP::RATE1_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(47.646, 74.418)), module, PSIOP::RATE2_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(7.528, 55.132)), module, PSIOP::SPEED_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(47.646, 90.854)), module, PSIOP::RATE2ATTEN_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(60.124, 90.854)), module, PSIOP::WAVEATTEN_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(72.824, 90.854)), module, PSIOP::RATIOATTEN_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.011, 20.994)), module, PSIOP::START_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(46.08, 21.213)), module, PSIOP::END_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(72.824, 108.2)), module, PSIOP::RATIO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(60.124, 108.2)), module, PSIOP::WAVE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.493, 90.854)), module, PSIOP::ALGO_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(85.997, 90.854)), module, PSIOP::FB_INPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.457, 90.854)), module, PSIOP::RATE1_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.646, 108.2)), module, PSIOP::RATE2_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.528, 90.854)), module, PSIOP::SPEED_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.528, 108.2)), module, PSIOP::TRIGGER_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.493, 108.2)), module, PSIOP::ACCENT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(33.457, 108.2)), module, PSIOP::CHOKE_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(83.818, 108.2)), module, PSIOP::OUT_OUTPUT));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.818, 118.2)), module, PSIOP::DEBUG1_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.818, 118.2)), module, PSIOP::DEBUG2_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(50.818, 118.2)), module, PSIOP::DEBUG3_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(60.818, 118.2)), module, PSIOP::DEBUG4_OUTPUT));

        addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(80.744, 101.484)), module, PSIOP::OUT_LIGHT));
    }
};

Model *modelPSIOP = createModel<PSIOP, PSIOPWidget>("PSIOP");