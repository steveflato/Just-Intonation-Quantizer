/*
 * JI Quantizer - Just Intonation Quantizer for distingNT
 * GUID: SFJz
 * Author: Safie Flato
 * Category: Utility
 *
 * Variable 3-43 note just intonation scale. Note 0 is always 1/1.
 * Notes 1-(N-1) are user-defined ratios (numerator / denominator).
 * Supports up to 4 channels of CV I/O with independent gate options.
 */

#include <distingnt/api.h>
#include <math.h>
#include <string.h>
#include <new>

// ============================================================
// Constants
// ============================================================
static constexpr int   kMaxScaleNotes    = 43;
static constexpr int   kMaxUserNotes     = 42;  // kMaxScaleNotes - 1
static constexpr int   kMinNotes         = 3;
static constexpr int   kDefaultNotes     = 12;
static constexpr int   kMaxChannels      = 4;
static constexpr int   kMaxRatioVal      = 256;
static constexpr float kGateOnThreshold  = 2.5f; // gate opens above this
static constexpr float kGateOffThreshold = 0.5f; // gate closes below this (hysteresis)
static constexpr float kGateHighV        = 5.0f;

// Display (256x64, kNT_textNormal = 8px tall)
static constexpr int kDisplayCols = 4;
static constexpr int kColWidth    = 64;
static constexpr int kDisplayRows = 3;
static constexpr int kRowHeight   = 9;
static constexpr int kRatioStartY = 26;

static const char* const kNotePageNames[kMaxUserNotes] = {
    "Note 1",  "Note 2",  "Note 3",  "Note 4",  "Note 5",
    "Note 6",  "Note 7",  "Note 8",  "Note 9",  "Note 10",
    "Note 11", "Note 12", "Note 13", "Note 14", "Note 15",
    "Note 16", "Note 17", "Note 18", "Note 19", "Note 20",
    "Note 21", "Note 22", "Note 23", "Note 24", "Note 25",
    "Note 26", "Note 27", "Note 28", "Note 29", "Note 30",
    "Note 31", "Note 32", "Note 33", "Note 34", "Note 35",
    "Note 36", "Note 37", "Note 38", "Note 39", "Note 40",
    "Note 41", "Note 42",
};

// ============================================================
// Specifications
// ============================================================
enum { kSpecChannels, kSpecNotes };
static const _NT_specification specifications[] = {
    { .name = "Channels", .min = 1,         .max = kMaxChannels,   .def = 1,            .type = kNT_typeGeneric },
    { .name = "Notes",    .min = kMinNotes,  .max = kMaxScaleNotes, .def = kDefaultNotes, .type = kNT_typeGeneric },
};

// ============================================================
// Shared parameters
// ============================================================
enum {
    kParamRoot = 0,
    kNumSharedParams,   // = 1
};

// Per-channel parameter offsets
enum {
    kChCV_In     = 0,
    kChCV_Out,
    kChGate_In,
    kChGate_Out,
    kChInputGate,   // bool: sample quantizer on gate rise
    kChOutputGate,  // bool: fire gate out on note change or received gate
    kParamsPerChannel,  // = 6
};

// ============================================================
// Index helpers
// ============================================================
static inline int chBase(int ch)                     { return kNumSharedParams + ch * kParamsPerChannel; }
static inline int ratioBase(int numCh)               { return kNumSharedParams + numCh * kParamsPerChannel; }
static inline int numIdx(int numCh, int i)           { return ratioBase(numCh) + i * 2; }
static inline int denIdx(int numCh, int i)           { return ratioBase(numCh) + i * 2 + 1; }
static inline int totalParams(int numCh, int numUN)  { return kNumSharedParams + numCh * kParamsPerChannel + numUN * 2; }
static inline int totalPages(int numCh, int numUN)   { return 1 + numCh + numUN; }

// ============================================================
// Defaults
// Slots  0-10: Kraig Grady's Centaur scale (preserves existing behavior at 12 notes)
// Slots 11-41: Harry Partch's 43-tone additions, sorted ascending pitch
//              (selecting 43 notes gives the full Partch scale)
// ============================================================
static const int16_t kDefNum[kMaxUserNotes] = {
    21,  9,  7,  5,  4,  7,  3, 14,  5,  7, 15,   // Centaur 1-11
    81, 33, 16, 12, 11, 10,  8, 32,  6, 11, 14,    // Partch: 81/80 .. 14/11
     9, 21, 27, 11, 10, 16, 40, 32, 11,  8, 18,    // Partch:  9/7  .. 18/11
    27, 12, 16,  9, 20, 11, 40, 64, 160,            // Partch: 27/16 .. 160/81
};
static const int16_t kDefDen[kMaxUserNotes] = {
    20,  8,  6,  4,  3,  5,  2,  9,  3,  4,  8,   // Centaur 1-11
    80, 32, 15, 11, 10,  9,  7, 27,  5,  9, 11,    // Partch: 81/80 .. 14/11
     7, 16, 20,  8,  7, 11, 27, 21,  7,  5, 11,    // Partch:  9/7  .. 18/11
    16,  7,  9,  5, 11,  6, 21, 33,  81,            // Partch: 27/16 .. 160/81
};

// ============================================================
// DTC
// ============================================================
struct _jiQuantizer_DTC {
    float scalePitch[kMaxScaleNotes];
    int   scaleSrcIdx[kMaxScaleNotes];
    int   numValidNotes;

    float rootOffsetV;

    float heldOutputV[kMaxChannels];
    int   currentDegree[kMaxChannels];
    float prevGate[kMaxChannels];

    // Display state (channel 0)
    float displayInputV;
    float displayOutputV;
    int   displayDegree;
};

// ============================================================
// Algorithm struct
// ============================================================
struct _jiQuantizerAlgorithm : public _NT_algorithm {
    _jiQuantizerAlgorithm() {}
    ~_jiQuantizerAlgorithm() {}

    _NT_parameter*     params;
    int                numParams;
    _NT_parameterPage* pages;
    int                numPages;
    uint8_t*           pageArrays;
    _NT_parameterPages paramPages;

    _jiQuantizer_DTC*  dtc;
    int                numChannels;
    int                numUserNotes;  // = Notes spec - 1

    char noteNameBufs[kMaxUserNotes][16]; // live page name strings, e.g. "N1: 3/2"
};

// ============================================================
// Scale table builder
// ============================================================
static void buildScaleTable(_jiQuantizerAlgorithm* a) {
    _jiQuantizer_DTC* d = a->dtc;

    float pitches[kMaxScaleNotes];
    int   srcIdx[kMaxScaleNotes];
    int   count = 0;

    // Note 0: always 1/1 = 0.0 V/oct
    pitches[0] = 0.0f;
    srcIdx[0]  = -1;
    count = 1;

    // Notes 1-(numUserNotes): user-defined
    for (int i = 0; i < a->numUserNotes; ++i) {
        int n   = a->v[numIdx(a->numChannels, i)];
        int den = a->v[denIdx(a->numChannels, i)];
        if (n   <= 0) n   = 1;
        if (den <= 0) den = 1;

        float p = logf((float)n / (float)den) * 1.44269504f; // log2(n/d)
        p = p - floorf(p); // fold into [0, 1)

        bool dup = false;
        for (int j = 0; j < count; ++j) {
            if (fabsf(pitches[j] - p) < 0.0002f) { dup = true; break; }
        }
        if (!dup) {
            pitches[count] = p;
            srcIdx[count]  = i;
            count++;
        }
    }

    // Insertion sort ascending
    for (int i = 1; i < count; ++i) {
        float tp = pitches[i];
        int   ts = srcIdx[i];
        int   j  = i - 1;
        while (j >= 0 && pitches[j] > tp) {
            pitches[j+1] = pitches[j];
            srcIdx[j+1]  = srcIdx[j];
            j--;
        }
        pitches[j+1] = tp;
        srcIdx[j+1]  = ts;
    }

    d->numValidNotes = count;
    for (int i = 0; i < count; ++i) {
        d->scalePitch[i]  = pitches[i];
        d->scaleSrcIdx[i] = srcIdx[i];
    }
}

// ============================================================
// Update note page labels to show current ratio, e.g. "N1: 3/2"
// ============================================================
static void updateNotePageNames(_jiQuantizerAlgorithm* a, bool notify) {
    int ch = a->numChannels;

    for (int i = 0; i < a->numUserNotes; ++i) {
        int num = a->v[numIdx(ch, i)];
        int den = a->v[denIdx(ch, i)];

        char* p = a->noteNameBufs[i];
        *p++ = 'N';
        p += NT_intToString(p, i + 1);
        *p++ = ':'; *p++ = ' ';
        p += NT_intToString(p, num);
        *p++ = '/';
        p += NT_intToString(p, den);
        *p = '\0';

        a->pages[1 + ch + i].name = a->noteNameBufs[i];
    }

    if (notify)
        NT_updateParameterPages(NT_algorithmIndex(a));
}

// ============================================================
// Quantize
// ============================================================
static float quantizePitch(const _jiQuantizer_DTC* d, float inputV, int* outDegree) {
    int n = d->numValidNotes;
    if (n <= 0) { *outDegree = 0; return inputV; }

    float pitch  = inputV - d->rootOffsetV;
    float octave = floorf(pitch);
    float frac   = pitch - octave;

    int   best     = 0;
    float bestDist = 1e9f;
    for (int i = 0; i < n; ++i) {
        float diff = frac - d->scalePitch[i];
        if (diff >  0.5f) diff -= 1.0f;
        if (diff < -0.5f) diff += 1.0f;
        float dist = fabsf(diff);
        if (dist < bestDist) { bestDist = dist; best = i; }
    }

    *outDegree = best;
    return d->rootOffsetV + octave + d->scalePitch[best];
}

// ============================================================
// calculateRequirements
// ============================================================
static void calculateRequirements(_NT_algorithmRequirements& req, const int32_t* specs) {
    int ch   = specs ? specs[kSpecChannels] : 1;
    int numN = specs ? specs[kSpecNotes]    : kDefaultNotes;
    int numUN = numN - 1;
    int np    = totalParams(ch, numUN);
    int npg   = totalPages(ch, numUN);

    // Page array bytes: scale(1) + routing(ch*6) + note pages(numUN*2)
    int pageBytes = 1 + ch * kParamsPerChannel + numUN * 2;

    req.numParameters = np;
    req.sram = sizeof(_jiQuantizerAlgorithm)
             + np  * sizeof(_NT_parameter)
             + npg * sizeof(_NT_parameterPage)
             + pageBytes;
    req.dram = 0;
    req.dtc  = sizeof(_jiQuantizer_DTC);
    req.itc  = 0;
}

// ============================================================
// construct
// ============================================================
static _NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                                const _NT_algorithmRequirements&,
                                const int32_t* specs) {
    int ch   = specs ? specs[kSpecChannels] : 1;
    int numN = specs ? specs[kSpecNotes]    : kDefaultNotes;
    int numUN = numN - 1;
    int np    = totalParams(ch, numUN);
    int npg   = totalPages(ch, numUN);

    _jiQuantizerAlgorithm* alg = new (ptrs.sram) _jiQuantizerAlgorithm();
    uint8_t* mem = (uint8_t*)ptrs.sram + sizeof(_jiQuantizerAlgorithm);

    alg->params       = (_NT_parameter*)mem;     mem += np  * sizeof(_NT_parameter);
    alg->numParams    = np;
    alg->pages        = (_NT_parameterPage*)mem; mem += npg * sizeof(_NT_parameterPage);
    alg->numPages     = npg;
    alg->pageArrays   = mem;
    alg->numChannels  = ch;
    alg->numUserNotes = numUN;

    // ---- Shared params ----
    alg->params[kParamRoot] = { .name = "Root", .min = -24, .max = 24, .def = 0,
                                .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL };

    // ---- Per-channel params ----
    for (int c = 0; c < ch; ++c) {
        int b = chBase(c);
        alg->params[b + kChCV_In]     = { .name = "CV In",       .min = 1, .max = 28, .def = (int16_t)(1 + c),
                                          .unit = kNT_unitAudioInput,  .scaling = 0, .enumStrings = NULL };
        alg->params[b + kChCV_Out]    = { .name = "CV Out",      .min = 1, .max = 28, .def = (int16_t)(13 + c),
                                          .unit = kNT_unitAudioOutput, .scaling = 0, .enumStrings = NULL };
        alg->params[b + kChGate_In]   = { .name = "Gate In",     .min = 1, .max = 28, .def = (int16_t)(2 + c),
                                          .unit = kNT_unitAudioInput,  .scaling = 0, .enumStrings = NULL };
        alg->params[b + kChGate_Out]  = { .name = "Gate Out",    .min = 1, .max = 28, .def = (int16_t)(14 + c),
                                          .unit = kNT_unitAudioOutput, .scaling = 0, .enumStrings = NULL };
        alg->params[b + kChInputGate] = { .name = "Input Gate",  .min = 0, .max = 1,  .def = 0,
                                          .unit = kNT_unitNone,        .scaling = 0, .enumStrings = NULL };
        alg->params[b + kChOutputGate]= { .name = "Output Gate", .min = 0, .max = 1,  .def = 0,
                                          .unit = kNT_unitNone,        .scaling = 0, .enumStrings = NULL };
    }

    // ---- Ratio params: notes 1-numUN ----
    int rb = ratioBase(ch);
    for (int i = 0; i < numUN; ++i) {
        alg->params[rb + i*2]   = { .name = "Numerator",   .min = 1, .max = kMaxRatioVal, .def = kDefNum[i],
                                    .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL };
        alg->params[rb + i*2+1] = { .name = "Denominator", .min = 1, .max = kMaxRatioVal, .def = kDefDen[i],
                                    .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL };
    }

    // ---- Pages ----
    uint8_t* pa = alg->pageArrays;

    // Page 0: Scale (Root)
    pa[0] = kParamRoot;
    alg->pages[0] = { .name = "Scale", .numParams = 1, .params = pa };
    pa += 1;

    // Pages 1..ch: Per-channel routing
    for (int c = 0; c < ch; ++c) {
        int b = chBase(c);
        for (int p = 0; p < kParamsPerChannel; ++p) pa[p] = (uint8_t)(b + p);
        alg->pages[1 + c] = { .name = "Routing", .numParams = (uint8_t)kParamsPerChannel, .params = pa };
        pa += kParamsPerChannel;
    }

    // Pages (1+ch)..(numUN+ch): one page per user note
    for (int i = 0; i < numUN; ++i) {
        pa[0] = (uint8_t)(rb + i*2);
        pa[1] = (uint8_t)(rb + i*2 + 1);
        alg->pages[1 + ch + i] = { .name = kNotePageNames[i], .numParams = 2, .params = pa };
        pa += 2;
    }

    alg->paramPages     = { .numPages = (uint32_t)npg, .pages = alg->pages };
    alg->parameters     = alg->params;
    alg->parameterPages = &alg->paramPages;

    // ---- Init DTC ----
    _jiQuantizer_DTC* d = (_jiQuantizer_DTC*)ptrs.dtc;
    alg->dtc = d;
    memset(d, 0, sizeof(_jiQuantizer_DTC));
    buildScaleTable(alg);
    updateNotePageNames(alg, false);

    return alg;
}

// ============================================================
// parameterChanged
// ============================================================
static void parameterChanged(_NT_algorithm* self, int p) {
    _jiQuantizerAlgorithm* a = (_jiQuantizerAlgorithm*)self;
    if (p == kParamRoot) {
        a->dtc->rootOffsetV = a->v[kParamRoot] / 12.0f;
        return;
    }
    if (p >= ratioBase(a->numChannels)) {
        buildScaleTable(a);
        updateNotePageNames(a, true);
    }
}

// ============================================================
// step
// ============================================================
static void step(_NT_algorithm* self, float* bus, int numFramesBy4) {
    _jiQuantizerAlgorithm* a = (_jiQuantizerAlgorithm*)self;
    _jiQuantizer_DTC*      d = a->dtc;
    int numFrames = numFramesBy4 * 4;

    for (int c = 0; c < a->numChannels; ++c) {
        int b = chBase(c);

        bool inputGate  = (bool)a->v[b + kChInputGate];
        bool outputGate = (bool)a->v[b + kChOutputGate];

        int cvInBus    = a->v[b + kChCV_In]   - 1;
        int cvOutBus   = a->v[b + kChCV_Out]  - 1;
        int gateInBus  = a->v[b + kChGate_In] - 1;
        int gateOutBus = a->v[b + kChGate_Out]- 1;

        const float* cvIn   = bus + cvInBus  * numFrames;
        float*       cvOut  = bus + cvOutBus * numFrames;
        const float* gateIn = (inputGate && gateInBus >= 0) ? bus + gateInBus * numFrames : nullptr;

        int   prevDegree  = d->currentDegree[c];
        float gateState   = d->prevGate[c]; // 0.0 = low, 1.0 = high

        for (int i = 0; i < numFrames; ++i) {
            float g = gateIn ? gateIn[i] : 0.0f;
            bool gateRising = false;

            if (gateIn) {
                if (gateState < 0.5f && g >= kGateOnThreshold) {
                    gateState  = 1.0f; // gate opened
                    gateRising = true;
                } else if (gateState >= 0.5f && g < kGateOffThreshold) {
                    gateState = 0.0f;  // gate closed
                }
            }

            bool triggered = !inputGate || !gateIn || gateRising;

            if (triggered) {
                int degree = 0;
                d->heldOutputV[c]   = quantizePitch(d, cvIn[i], &degree);
                d->currentDegree[c] = degree;
            }

            cvOut[i] = d->heldOutputV[c];
        }

        d->prevGate[c] = gateState;

        // Gate output
        if (outputGate && gateOutBus >= 0) {
            float* gateOut = bus + gateOutBus * numFrames;
            bool noteChanged = (d->currentDegree[c] != prevDegree);

            if (inputGate && gateIn) {
                // Pass gate signal through
                for (int i = 0; i < numFrames; ++i)
                    gateOut[i] = gateIn[i];
            } else if (!inputGate && noteChanged) {
                // Fire a one-block trigger on note change
                for (int i = 0; i < numFrames; ++i)
                    gateOut[i] = kGateHighV;
            } else {
                for (int i = 0; i < numFrames; ++i)
                    gateOut[i] = 0.0f;
            }
        }
    }

    // Update display from channel 0
    {
        const float* cvIn0 = bus + (a->v[chBase(0) + kChCV_In] - 1) * numFrames;
        d->displayInputV  = cvIn0[numFrames - 1];
        d->displayOutputV = d->heldOutputV[0];
        d->displayDegree  = d->currentDegree[0];
    }
}

// ============================================================
// draw helpers
// ============================================================
static void writeRatio(char* buf, int num, int den) {
    int len = NT_intToString(buf, num);
    buf[len++] = '/';
    len += NT_intToString(buf + len, den);
    buf[len] = '\0';
}

static void writeVoltageCompact(char* buf, float v) {
    char* p = buf;
    if (v < 0.0f) { *p++ = '-'; v = -v; } else { *p++ = '+'; }
    int intPart  = (int)v;
    int fracPart = (int)((v - (float)intPart) * 100.0f + 0.5f);
    if (fracPart >= 100) { intPart++; fracPart = 0; }
    p += NT_intToString(p, intPart);
    *p++ = '.';
    if (fracPart < 10) *p++ = '0';
    p += NT_intToString(p, fracPart);
    *p++ = 'V'; *p = '\0';
}

// ============================================================
// draw
// ============================================================
static bool draw(_NT_algorithm* self) {
    _jiQuantizerAlgorithm* a = (_jiQuantizerAlgorithm*)self;
    _jiQuantizer_DTC*      d = a->dtc;

    int rb        = ratioBase(a->numChannels);
    int n         = d->numValidNotes;
    int activeDeg = d->displayDegree;
    int root      = a->v[kParamRoot];

    // Gate indicators for channel 0
    bool ch0InputGate  = (bool)a->v[chBase(0) + kChInputGate];
    bool ch0OutputGate = (bool)a->v[chBase(0) + kChOutputGate];

    // ---- Header ----
    NT_drawText(0, 0, "JI Quantizer");

    char buf[32];
    int len = 0;
    if (root >= 0) buf[len++] = '+';
    len += NT_intToString(buf + len, root);
    buf[len++] = 's';
    if (ch0InputGate)  { buf[len++] = ' '; buf[len++] = 'I'; buf[len++] = 'G'; }
    if (ch0OutputGate) { buf[len++] = ' '; buf[len++] = 'O'; buf[len++] = 'G'; }
    buf[len] = '\0';
    NT_drawText(256 - len * 6, 0, buf);

    // ---- Voltages + active ratio ----
    writeVoltageCompact(buf, d->displayInputV);
    NT_drawText(0, 9, buf);

    buf[0] = '>'; buf[1] = ' ';
    writeVoltageCompact(buf + 2, d->displayOutputV);
    NT_drawText(90, 9, buf);

    if (n > 0 && activeDeg < n) {
        int src = d->scaleSrcIdx[activeDeg];
        char ratBuf[12];
        if (src < 0) {
            ratBuf[0]='1'; ratBuf[1]='/'; ratBuf[2]='1'; ratBuf[3]='\0';
        } else {
            writeRatio(ratBuf, a->v[rb + src*2], a->v[rb + src*2+1]);
        }
        NT_drawText(200, 9, ratBuf);
    }

    // ---- Ratio grid ----
    int visible     = kDisplayRows * kDisplayCols;
    int windowStart = activeDeg - visible / 2;
    if (windowStart < 0) windowStart = 0;
    if (windowStart + visible > n) windowStart = n - visible;
    if (windowStart < 0) windowStart = 0;

    for (int row = 0; row < kDisplayRows; ++row) {
        for (int col = 0; col < kDisplayCols; ++col) {
            int idx = windowStart + row * kDisplayCols + col;
            if (idx >= n) break;

            int src = d->scaleSrcIdx[idx];
            char ratBuf[12];
            if (src < 0) {
                ratBuf[0]='1'; ratBuf[1]='/'; ratBuf[2]='1'; ratBuf[3]='\0';
            } else {
                writeRatio(ratBuf, a->v[rb + src*2], a->v[rb + src*2+1]);
            }

            int x = col * kColWidth;
            int y = kRatioStartY + row * kRowHeight;

            if (idx == activeDeg) {
                NT_drawShapeI(kNT_rectangle, x, y, x + kColWidth - 2, y + 8, 15);
                NT_drawText(x + 2, y, ratBuf, 0);
            } else {
                NT_drawText(x + 2, y, ratBuf);
            }
        }
    }

    // Scroll indicator
    if (n > visible) {
        int barH  = 64 - kRatioStartY;
        int tickY = kRatioStartY + (activeDeg * barH) / (n > 1 ? n - 1 : 1);
        NT_drawShapeI(kNT_rectangle, 254, kRatioStartY, 255, kRatioStartY + barH, 6);
        NT_drawShapeI(kNT_rectangle, 253, tickY - 1,    255, tickY + 2,            15);
    }

    return false;
}

// ============================================================
// Factory
// ============================================================
static const _NT_factory factory = {
    .guid                        = NT_MULTICHAR('S', 'F', 'J', 'z'),
    .name                        = "JI Quantizer",
    .description                 = "3-43 note just intonation quantizer with user-defined ratios",
    .numSpecifications           = ARRAY_SIZE(specifications),
    .specifications              = specifications,
    .calculateStaticRequirements = NULL,
    .initialise                  = NULL,
    .calculateRequirements       = calculateRequirements,
    .construct                   = construct,
    .parameterChanged            = parameterChanged,
    .step                        = step,
    .draw                        = draw,
    .midiRealtime                = NULL,
    .midiMessage                 = NULL,
    .tags                        = kNT_tagUtility,
    .hasCustomUi                 = NULL,
    .customUi                    = NULL,
    .setupUi                     = NULL,
    .serialise                   = NULL,
    .deserialise                 = NULL,
    .midiSysEx                   = NULL,
    .parameterUiPrefix           = NULL,
    .parameterString             = NULL,
};

uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
    case kNT_selector_version:      return kNT_apiVersion13;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)((data == 0) ? &factory : NULL);
    }
    return 0;
}
