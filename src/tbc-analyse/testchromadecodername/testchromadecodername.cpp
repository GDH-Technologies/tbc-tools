/******************************************************************************
 * testchromadecodername.cpp
 * tbc-analyse - NTSC adaptive state in the chroma decoder name
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 GDH-Technologies LLC
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

// tbc-chroma-decoder ignores the metadata's ntscAdaptive once -f names the
// decoder, and an export always names it. These tests hold the name to the
// adaptive state, so an export decodes like the preview.

// Tests must assert even in Release builds, where NDEBUG would otherwise
// compile every assert() away
#undef NDEBUG
#include <cassert>
#include <iostream>

#include <QCoreApplication>

#include "tbcsource.h"

namespace {

QString nameFor(VideoSystem system, qint32 dimensions, bool adaptive, bool nnTransform3D)
{
    PalColour::Configuration palConfig;
    Comb::Configuration ntscConfig;
    ntscConfig.dimensions = dimensions;
    ntscConfig.adaptive = adaptive;
    ntscConfig.nnTransform3D = nnTransform3D;
    return TbcSource::chromaDecoderName(system, palConfig, ntscConfig);
}

TbcMetaData::VideoParameters ntscParameters(const QString &decoder, qint32 ntscAdaptive)
{
    TbcMetaData::VideoParameters videoParameters;
    videoParameters.system = NTSC;
    videoParameters.chromaDecoder = decoder;
    videoParameters.ntscAdaptive = ntscAdaptive;
    return videoParameters;
}

void testNameFromConfiguration()
{
    assert(nameFor(NTSC, 3, true, false) == QLatin1String("ntsc3d"));
    assert(nameFor(NTSC, 3, false, false) == QLatin1String("ntsc3dnoadapt"));

    // Adaptive belongs to the classic 3D decoder alone
    assert(nameFor(NTSC, 3, false, true) == QLatin1String("nntransform3d"));
    assert(nameFor(NTSC, 2, false, false) == QLatin1String("ntsc2d"));
    assert(nameFor(NTSC, 1, false, false) == QLatin1String("ntsc1d"));
    assert(nameFor(NTSC, 0, false, false) == QLatin1String("mono"));
}

void testStoredNameSwitchesAdaptiveOff()
{
    // The name wins over the field, as it does in the decoder
    TbcSource source(nullptr);
    source.setVideoParameters(ntscParameters(QStringLiteral("ntsc3dnoadapt"), 1));
    assert(source.getNtscConfiguration().dimensions == 3);
    assert(!source.getNtscConfiguration().adaptive);
    assert(!source.getNtscConfiguration().nnTransform3D);
    assert(source.getChromaDecoderName() == QLatin1String("ntsc3dnoadapt"));
}

void testNoStoredNameTakesAdaptiveFromField()
{
    TbcSource source(nullptr);
    source.setVideoParameters(ntscParameters(QString(), 0));
    assert(source.getVideoParametersWithViewChroma().chromaDecoder == QLatin1String("ntsc3dnoadapt"));

    source.setVideoParameters(ntscParameters(QString(), 1));
    assert(source.getVideoParametersWithViewChroma().chromaDecoder == QLatin1String("ntsc3d"));
}

void testExportHonoursAdaptiveOffOnStoredNtsc3d()
{
    // Metadata saved before the name carried adaptive
    TbcSource source(nullptr);
    source.setVideoParameters(ntscParameters(QStringLiteral("ntsc3d"), 0));
    assert(source.getVideoParametersWithViewChroma().chromaDecoder == QLatin1String("ntsc3dnoadapt"));

    // The export's copy changes; the metadata doesn't
    assert(source.getVideoParameters().chromaDecoder == QLatin1String("ntsc3d"));
}

void testExportLeavesOtherDecodersAlone()
{
    TbcSource source(nullptr);

    source.setVideoParameters(ntscParameters(QStringLiteral("ntsc3d"), 1));
    assert(source.getVideoParametersWithViewChroma().chromaDecoder == QLatin1String("ntsc3d"));

    // -1: the metadata doesn't say, and the decoder's default is adaptive
    source.setVideoParameters(ntscParameters(QStringLiteral("ntsc3d"), -1));
    assert(source.getVideoParametersWithViewChroma().chromaDecoder == QLatin1String("ntsc3d"));

    source.setVideoParameters(ntscParameters(QStringLiteral("ntsc2d"), 0));
    assert(source.getVideoParametersWithViewChroma().chromaDecoder == QLatin1String("ntsc2d"));

    source.setVideoParameters(ntscParameters(QStringLiteral("nntransform3d"), 0));
    assert(source.getVideoParametersWithViewChroma().chromaDecoder == QLatin1String("nntransform3d"));

    TbcMetaData::VideoParameters palParameters = ntscParameters(QStringLiteral("transform3d"), 0);
    palParameters.system = PAL;
    source.setVideoParameters(palParameters);
    assert(source.getVideoParametersWithViewChroma().chromaDecoder == QLatin1String("transform3d"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testNameFromConfiguration();
    testStoredNameSwitchesAdaptiveOff();
    testNoStoredNameTakesAdaptiveFromField();
    testExportHonoursAdaptiveOffOnStoredNtsc3d();
    testExportLeavesOtherDecodersAlone();

    std::cout << "testchromadecodername: all tests passed\n";
    return 0;
}
