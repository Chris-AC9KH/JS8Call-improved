#include <QLoggingCategory>
#include <QTimeZone>
#include <stdexcept>
#include <boost/format.hpp>
#include "DriftingDateTime.h"
#include "TxLoop.h"
#include "JS8Submode.hpp"

Q_DECLARE_LOGGING_CATEGORY(txloop_js8)

TxLoop::TxLoop(const QString & name) :
    m_name{name},
    m_next_activity{DriftingDateTime::currentDateTimeUtc()},
    m_tx_delay_ms{100}, // arbitrary
    m_submode{Varicode::JS8CallSlow},
    m_active{false},
    m_loop_period_ms{600000} // arbitrary
{
    m_next_activity_timer.setTimerType(Qt::PreciseTimer);
    m_next_activity_timer.setSingleShot(true);
    connect(&m_next_activity_timer, &QTimer::timeout, this, &TxLoop::onTimer);
}

TxLoop::~TxLoop() {
    onLoopCancel();
    qCDebug(txloop_js8) << m_name << "Destruction of TX loop";
}

void TxLoop::onTimer() {
    m_next_activity_timer.stop();
    if(m_active) {
        // Calculate the next start point.
        // Doing it as milliseconds is fast and straightforward.
        qint64 next_activity_ms_raw = m_next_activity.toMSecsSinceEpoch() + m_loop_period_ms;
        qint64 mode_period_ms = ((qint64)1000) * JS8::Submode::period(m_submode);

        // Make sure that we start when a node slot starts:
        qint64 into_mode_slot =  next_activity_ms_raw % mode_period_ms;
        if (0 != into_mode_slot)
            qCWarning(txloop_js8)
                << m_name << "in onTimer callback, next activity planned for"
                << QDateTime::fromMSecsSinceEpoch(next_activity_ms_raw, QTimeZone::utc())
                << "did not fit mode_period of" << mode_period_ms << "ms, so moving"
                << into_mode_slot << "towards the past";
        qint64 next_activity_ms = next_activity_ms_raw - into_mode_slot;

        // Be a bit paranoid and make sure we wait for the future, not for the past:
        qint64 now_ms = DriftingDateTime::currentMSecsSinceEpoch();
        if(next_activity_ms - m_tx_delay_ms < now_ms) {
            qint64 loop_periods_missing = (now_ms - (next_activity_ms - m_tx_delay_ms)) / m_loop_period_ms + 1;
            qint64 additional_increase = loop_periods_missing * m_loop_period_ms;
            qCWarning(txloop_js8)
                << m_name << "next activity planned for"
                << QDateTime::fromMSecsSinceEpoch(next_activity_ms, QTimeZone::utc())
                << "minus tx_delay of "<< m_tx_delay_ms << "ms has been found to be in the past, re-scheduling"
                << additional_increase << "ms into the future";
            next_activity_ms += additional_increase;
        }

        qint64 wait_for_next_trigger = next_activity_ms - now_ms - m_tx_delay_ms;
        m_next_activity.setMSecsSinceEpoch(next_activity_ms);
        qCDebug(txloop_js8)
            << m_name << "triggers TX, and also plans future signal to happen at" << m_next_activity
            << ". Taking into account TX delay" << m_tx_delay_ms << "ms, the timer is to wake us in" << wait_for_next_trigger << "ms";
        m_next_activity_timer.start(wait_for_next_trigger);
        emit nextActivityChanged(m_next_activity);
        emit triggerTxNow();
    } else {
        qCDebug(txloop_js8) << m_name << "timer triggered, but loop is inactive, so ignoring.";
    }
}

void TxLoop::onPlumbingCompleted() const {
    if(m_active) {
        qCDebug(txloop_js8)
            << m_name
            << "Internal signal plumbing completed. "
            "Telling all who want to know that the next transmission has been scheduled to occur at" << m_next_activity;
        emit nextActivityChanged(m_next_activity);
    } else {
        qCDebug(txloop_js8)
            << m_name << "Internal signal plumbing completed.  Telling all who want to know I'm idle with not transmission planned.";
        emit canceled();
    }
}

void TxLoop::onDriftChange(qint64 /* new_drift */) {
    if (m_active) {
        m_next_activity_timer.stop();
        QDateTime now = DriftingDateTime::currentDateTimeUtc();
        QDateTime need_to_push_ptt = m_next_activity.addMSecs(-m_tx_delay_ms);
        if (now < need_to_push_ptt && need_to_push_ptt < now.addMSecs(m_loop_period_ms)) {
            // Small drift change which does not affect when the next event is to happen.
            // Just adjust the timer (slightly).
            qint64 fire_in_ms = now.msecsTo(need_to_push_ptt);
            qCDebug(txloop_js8) << m_name << "Small drift change, pushing PTT in" << fire_in_ms << "ms";
            m_next_activity_timer.start(fire_in_ms);
        } else {
            qCDebug(txloop_js8) << m_name << "After big drift change, need to restart transmission schedule from square one.";
            onTxLoopPeriodChangeStart(m_loop_period_ms);
        }
    }
}

void TxLoop::onModeChange(Varicode::SubmodeType new_submode) {
    if (m_submode != new_submode) {
        qCDebug(txloop_js8)
            << m_name << "Submode change from" << JS8::Submode::name(m_submode)
            << "to" << JS8::Submode::name(new_submode) << ", restarting transmission schedule from square one.";
        const qint64 submode_period_ms = ((qint64)1000) * JS8::Submode::period(new_submode);
        if(m_loop_period_ms < submode_period_ms)
            throw std::invalid_argument{
                boost::str(boost::format("Submode period of %1%ms does not fit tx schedule period of %2%ms.")
                           % submode_period_ms % m_loop_period_ms)
            };
        m_submode = new_submode;
        if(m_active)
            onTxLoopPeriodChangeStart(m_loop_period_ms);
    }
}

void TxLoop::onTxDelayChange(qint64 tx_delay_ms) {
    qCDebug(txloop_js8)
        << m_name << "TX delay change from" << this->m_tx_delay_ms
        << "to" << tx_delay_ms;

    if(MAX_TX_DELAY_MS < tx_delay_ms)
        throw std::invalid_argument{
            boost::str(
                       boost::format("Unreasonably long tx delay of %1%ms not acceptable, max is %2%.")
                       % tx_delay_ms
                       % MAX_TX_DELAY_MS
                       )};
    if(tx_delay_ms < 0)
        throw std::invalid_argument{boost::str(boost::format("Negative tx delay of %1%ms not acceptable.") % tx_delay_ms)};
    if(m_tx_delay_ms != tx_delay_ms) {
        m_tx_delay_ms = tx_delay_ms;

        if(m_active)
            onTxLoopPeriodChangeStart(m_loop_period_ms);
    }
}

void TxLoop::onLoopCancel() {
    if(m_active) {
        m_active = false;
        m_next_activity_timer.stop();
        qCDebug(txloop_js8) << m_name << "Canceling scheduled TX activity (and telling all who want to know).";
        emit canceled();
    } else {
        qCDebug(txloop_js8) << m_name << "I've been asked to cancel activity, but I was idle anyway.";
    }
}

void TxLoop::onTxLoopPeriodChangeStart(qint64 loop_period_ms) {
    // This is also sometimes used to adjust the timer when tx delay changed.
    m_next_activity_timer.stop(); // Probably redundant.
    m_loop_period_ms = loop_period_ms;
    QDateTime now = DriftingDateTime::currentDateTimeUtc();
    qint64 now_ms = now.currentMSecsSinceEpoch();
    qint64 earliest = now_ms + m_loop_period_ms;
    qint64 mode_period_ms = ((qint64)1000) * JS8::Submode::period(m_submode);
    qint64 remainder = earliest % mode_period_ms;
    qint64 next_start =
        0 == remainder ? earliest : earliest + mode_period_ms - remainder;
    bool need_to_send_signal =
        !m_active || next_start != m_next_activity.currentMSecsSinceEpoch();
    qint64 need_to_wait = next_start-now_ms-m_tx_delay_ms;
    m_active = true;
    m_next_activity.setMSecsSinceEpoch(next_start);
    m_next_activity_timer.start(need_to_wait);
    qCDebug(txloop_js8)
        << m_name
        << "Active tx loop, transmission to start at" << m_next_activity
        << (need_to_send_signal ?
            "newly scheduled, so telling all who want to know." :
            "but this is nothing new.")
        << "Need to become busy in" << need_to_wait
        << "ms for tx_delay_ms" << m_tx_delay_ms
        << "loop_period_ms" << m_loop_period_ms
        << "mode_period_ms" << mode_period_ms;
        // << "now_ms" << now_ms
        // << "earliest" << earliest
        // << "remainder" << remainder;

    if(need_to_send_signal)
        emit nextActivityChanged(m_next_activity);
}

Q_LOGGING_CATEGORY(txloop_js8, "txloop.js8", QtWarningMsg)
