/**
 * @file MetaDataRegistry.cpp
 * @brief Implementation of meta data registers
 */

#include "MetaDataRegistry.h"
#include "FrequencyLineEdit.h"
#include "FrequencyList.h"
#include "IARURegions.h"
#include "JS8_Audio/AudioDevice.h"
#include "JS8_Transceiver/Transceiver.h"
#include "JS8_Transceiver/TransceiverFactory.h"
#include "JS8_UI/Configuration.h"
#include "JS8_UI/WF.h"
#include "Radio.h"
#include "StationList.h"

#include <QItemEditorFactory>
#include <QMetaType>
#include <QStandardItemEditorCreator>

QItemEditorFactory *item_editor_factory() {
    static QItemEditorFactory *our_item_editor_factory = new QItemEditorFactory;
    return our_item_editor_factory;
}

void register_types() {
    // types in Radio.h are registered in their own translation unit
    // as they are needed in the wsjtx_udp shared library too

    // we still have to register the fully qualified names of enum types
    // used as signal/slot connection arguments since the new Qt 5.5
    // Q_ENUM macro only seems to register the unqualified name

    item_editor_factory()->registerEditor(
        qMetaTypeId<Radio::Frequency>(),
        new QStandardItemEditorCreator<FrequencyLineEdit>());
    // auto frequency_delta_type_id = qRegisterMetaType<Radio::FrequencyDelta>
    // ("FrequencyDelta");
    item_editor_factory()->registerEditor(
        qMetaTypeId<Radio::FrequencyDelta>(),
        new QStandardItemEditorCreator<FrequencyDeltaLineEdit>());

    // V100 Frequency list model
    qRegisterMetaType<FrequencyList_v2::Item>("Item_v2");
    qRegisterMetaType<FrequencyList_v2::FrequencyItems>("FrequencyItems_v2");
    qRegisterMetaType<FrequencyList_v3::Item>("Item_v3");
    qRegisterMetaType<FrequencyList_v3::FrequencyItems>("FrequencyItems_v3");

    // Audio device
    qRegisterMetaType<AudioDevice::Channel>("AudioDevice::Channel");

    // Configuration
    qRegisterMetaType<Configuration::DataMode>("Configuration::DataMode");

    // Station details
    qRegisterMetaType<StationList::Station>("Station");
    qRegisterMetaType<StationList::Stations>("Stations");

    // Transceiver
    qRegisterMetaType<Transceiver::TransceiverState>(
        "Transceiver::TransceiverState");
    qRegisterMetaType<Transceiver::MODE>("Transceiver::MODE");

    // Transceiver factory
    qRegisterMetaType<TransceiverFactory::DataBits>(
        "TransceiverFactory::DataBits");
    qRegisterMetaType<TransceiverFactory::StopBits>(
        "TransceiverFactory::StopBits");
    qRegisterMetaType<TransceiverFactory::Handshake>(
        "TransceiverFactory::Handshake");
    qRegisterMetaType<TransceiverFactory::PTTMethod>(
        "TransceiverFactory::PTTMethod");
    qRegisterMetaType<TransceiverFactory::TXAudioSource>(
        "TransceiverFactory::TXAudioSource");
    qRegisterMetaType<TransceiverFactory::SplitMode>(
        "TransceiverFactory::SplitMode");

    // Waterfall
    qRegisterMetaType<WF::Spectrum>("Spectrum");
    qRegisterMetaType<WF::Palette::Colours>("Colours");

    // IARURegions
    qRegisterMetaType<IARURegions::Region>("IARURegions::Region");
}
