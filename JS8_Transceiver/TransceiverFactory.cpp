/**
 * @file TransceiverFactory.cpp
 * @brief Implementation of the TransceiverFactory class
 */

#include "TransceiverFactory.h"
#include "DXLabSuiteCommanderTransceiver.h"
#include "EmulateSplitTransceiver.h"
#include "HRDTransceiver.h"
#include "HamlibTransceiver.h"
#include "TCITransceiver.h"

#include <QMetaType>

#include "moc_TransceiverFactory.cpp"

// we use the hamlib "Hamlib Dummy" transceiver for non-CAT radios,
// this allows us to still use the hamlib PTT control features for a
// unified PTT control solution

char const *const TransceiverFactory::basic_transceiver_name_ = "None";

namespace {
enum // supported non-hamlib radio interfaces
{
    NonHamlibBaseId = 9899,
    CommanderId,
    HRDId,
    TCIId,
    OmniRigOneId,
    OmniRigTwoId
};
}

TransceiverFactory::TransceiverFactory(TCISession *tci_session)
    : tci_session_{tci_session}
{
    HamlibTransceiver::register_transceivers(&transceivers_);
    DXLabSuiteCommanderTransceiver::register_transceivers(&transceivers_,
                                                          CommanderId);
    HRDTransceiver::register_transceivers(&transceivers_, HRDId);

    transceivers_[TCITransceiver::transceiver_name_] =
        Capabilities{
            TCIId,                 // model_number
            Capabilities::network, //port_type
            true,                  // has_CAT_PTT
            false,                 // has_CAT_PTT_mic_data
            false,                 // has_CAT_indirect_serial_PTT
            false                  // asynchronous (PollingTransceiver for now)
        };
}

TransceiverFactory::~TransceiverFactory() {
    HamlibTransceiver::unregister_transceivers();
}

auto TransceiverFactory::supported_transceivers() const
    -> Transceivers const & {
    return transceivers_;
}

auto TransceiverFactory::CAT_port_type(QString const &name) const
    -> Capabilities::PortType {
    return supported_transceivers()[name].port_type_;
}

bool TransceiverFactory::has_CAT_PTT(QString const &name) const {
    return supported_transceivers()[name].has_CAT_PTT_ ||
           supported_transceivers()[name].model_number_ > NonHamlibBaseId;
}

bool TransceiverFactory::has_CAT_PTT_mic_data(QString const &name) const {
    return supported_transceivers()[name].has_CAT_PTT_mic_data_;
}

bool TransceiverFactory::has_CAT_indirect_serial_PTT(
    QString const &name) const {
    return supported_transceivers()[name].has_CAT_indirect_serial_PTT_;
}

bool TransceiverFactory::has_asynchronous_CAT(QString const &name) const {
    return supported_transceivers()[name].asynchronous_;
}

void TransceiverFactory::set_tci_session(TCISession *session)
{
    tci_session_ = session;
}

std::unique_ptr<Transceiver>
TransceiverFactory::create(ParameterPack const &params,
                           QThread *target_thread) {
    qWarning() << "TransceiverFactory TCI create"
           << "factory=" << this
           << "tci_session=" << tci_session_
           << "target_thread=" << target_thread
           << "network_port=" << params.network_port;

    std::unique_ptr<Transceiver> result;
    switch (supported_transceivers()[params.rig_name].model_number_) {
    case CommanderId: {
        std::unique_ptr<TransceiverBase> basic_transceiver;
        if (PTT_method_CAT != params.ptt_type) {
            // we start with a dummy HamlibTransceiver object instance that can
            // support direct PTT
            basic_transceiver.reset(
                new HamlibTransceiver{params.ptt_type, params.ptt_port});
            if (target_thread) {
                basic_transceiver.get()->moveToThread(target_thread);
            }
        }

        // wrap the basic Transceiver object instance with a decorator object
        // that talks to DX Lab Suite Commander
        result.reset(new DXLabSuiteCommanderTransceiver{
            std::move(basic_transceiver), params.network_port,
            PTT_method_CAT == params.ptt_type, params.poll_interval});
        if (target_thread) {
            result->moveToThread(target_thread);
        }
    } break;

    case HRDId: {
        std::unique_ptr<TransceiverBase> basic_transceiver;
        if (PTT_method_CAT != params.ptt_type) {
            // we start with a dummy HamlibTransceiver object instance that can
            // support direct PTT
            basic_transceiver.reset(
                new HamlibTransceiver{params.ptt_type, params.ptt_port});
            if (target_thread) {
                basic_transceiver.get()->moveToThread(target_thread);
            }
        }

        // wrap the basic Transceiver object instance with a decorator object
        // that talks to ham Radio Deluxe
        result.reset(new HRDTransceiver{
            std::move(basic_transceiver), params.network_port,
            PTT_method_CAT == params.ptt_type, params.audio_source,
            params.poll_interval});
        if (target_thread) {
            result->moveToThread(target_thread);
        }
    } break;

    case TCIId: {
        if (!tci_session_) {
            throw error{
                tr("TCI session is not available. TCI cannot be tested from this context.")
            };
        }

        result.reset(new TCITransceiver{params, tci_session_});
        if (target_thread) {
            result->moveToThread(target_thread);
        }
    } break;

    default:
        result.reset(new HamlibTransceiver{
            supported_transceivers()[params.rig_name].model_number_, params});
        if (target_thread) {
            result->moveToThread(target_thread);
        }
        break;
    }

    if (split_mode_emulate == params.split_mode) {
        // wrap the Transceiver object instance with a decorator that emulates
        // split mode
        result.reset(new EmulateSplitTransceiver{std::move(result)});
        if (target_thread) {
            result->moveToThread(target_thread);
        }
    }

    return result;
}

#if !defined(QT_NO_DEBUG_STREAM)
ENUM_QDEBUG_OPS_IMPL(TransceiverFactory, DataBits);
ENUM_QDEBUG_OPS_IMPL(TransceiverFactory, StopBits);
ENUM_QDEBUG_OPS_IMPL(TransceiverFactory, Handshake);
ENUM_QDEBUG_OPS_IMPL(TransceiverFactory, PTTMethod);
ENUM_QDEBUG_OPS_IMPL(TransceiverFactory, TXAudioSource);
ENUM_QDEBUG_OPS_IMPL(TransceiverFactory, SplitMode);
#endif

ENUM_QDATASTREAM_OPS_IMPL(TransceiverFactory, DataBits);
ENUM_QDATASTREAM_OPS_IMPL(TransceiverFactory, StopBits);
ENUM_QDATASTREAM_OPS_IMPL(TransceiverFactory, Handshake);
ENUM_QDATASTREAM_OPS_IMPL(TransceiverFactory, PTTMethod);
ENUM_QDATASTREAM_OPS_IMPL(TransceiverFactory, TXAudioSource);
ENUM_QDATASTREAM_OPS_IMPL(TransceiverFactory, SplitMode);

ENUM_CONVERSION_OPS_IMPL(TransceiverFactory, DataBits);
ENUM_CONVERSION_OPS_IMPL(TransceiverFactory, StopBits);
ENUM_CONVERSION_OPS_IMPL(TransceiverFactory, Handshake);
ENUM_CONVERSION_OPS_IMPL(TransceiverFactory, PTTMethod);
ENUM_CONVERSION_OPS_IMPL(TransceiverFactory, TXAudioSource);
ENUM_CONVERSION_OPS_IMPL(TransceiverFactory, SplitMode);
