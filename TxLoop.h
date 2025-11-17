#ifndef TXLOOP_H
#define TXLOOP_H

#include <QDateTime>
#include <QTimer>

#include "TwoPhaseSignal.h"
#include "varicode.h"

/**
 * Class to organize transmit loops, i.e., regular schedule of repeated transmissions.
 * Intended use cases are either repeated CQ or HB transmissions.
 *
 * One object of this class organizes one such transmit loop.
 *
 * This class knows no other time than what DriftingDateTime tells it.
 **/
class TxLoop : public TwoPhaseSignal {
    Q_OBJECT

private:
    // Name, only used for logging:
    const QString m_name;
    // When do we need to start transmitting the next time?
    QDateTime m_next_activity;
    // How much tx_delay, in milliseconds, we need to be early.
    qint64 m_tx_delay_ms;
    // A timer that is supposed to wake us up when it is time.
    // This will be m_tx_delay_ms earlier than m_next_activity.
    QTimer m_next_activity_timer;
    // The currently active JS8 m_submode.
    Varicode::SubmodeType m_submode;
    // Whether this loop is active (or else does nothing).
    bool m_active;
    // The loop's period: After how many milliseconds to repeat sending.
    qint64 m_loop_period_ms;

public:
    // Provide a "name" argument that can be used for logging.
    TxLoop(const QString& name);

    ~TxLoop();

    // The TX delay cannot be set to any value larger than this:
    static constexpr qint64 MAX_TX_DELAY_MS = 1000;

    inline bool isActive() const { return m_active; }

    /**
     * Return the timestamp when the payload signal is supposed to start.
     * This timestamp will always point to a full second.
     * The actual triggering happens tx_delay earlier.
     *
     * The value this returns is undetermined (arbitrary) when called while isActive() returns false.
     */
    inline const QDateTime & nextActivity() const {
        return m_next_activity;
    }

    /**
     * Return the present period of the loop, in milliseconds, if isActive() returns true.
     *
     * Not sure this is useful, but: If isActive() returns false,
     * this will hand out the previous period used, or,
     * if this loop object has never been active, some reasonable initial value.
     */
    inline qint64 period_ms() const {
        return m_loop_period_ms;
    }

signals:
    /** Tells whoever wants to know that now is the time to push the PTT for TX delay: */
    void triggerTxNow() const;

    /**
     * Tells whoever wants to know that our future plans have changed.  Only an active loop will emit this.
     *
     * This signal is emitted in conjunction with triggerTxNow and also each time
     * our onTxLoopPeriodChangeStart receives a signal.
     *
     * The QDateTime sent is the same that will be returned by nextActivity().
     *
     * In addition, it gets fired when some other incoming onXXX signal caused
     * our next activity to change.
     */
    void nextActivityChanged(const QDateTime &) const;

    /** Tells whoever wants to know that we have become inactive. */
    void canceled() const;

public slots:
    /** Asks us to emit initial signals, either nextActivityChanged or, more likely, canceled. */
    void onPlumbingCompleted() const;
    /** Tells us that the DriftingDateTime has a new drift value.  We'll adjust accordingly. Idempotent. */
    void onDriftChange(qint64 new_drift);
    /** Tells us that the next transmission organized by this loop will be in this new submode. Idempotent. */
    void onModeChange(Varicode::SubmodeType new_submode);
    /**
     * Tells us that the desired TX delay
     * (switch TX on a bit before the full second that starts the mode period)
     * has changed. Idempotent.
     */
    void onTxDelayChange(qint64 tx_delay_ms);

    /**
     * This starts or restarts the loop.
     * You need to provide the desired period.
     *
     * After this signal has come in, isActive() will return true
     * and triggerTxNow will fire every loop_period_ms milliseconds.
     * This continues until the onLoopCancel signal is received.
     *
     * Calling this on a loop that is active will cause the schedule
     * to be reset; i.e., this restarts the loop.
     */
    void onTxLoopPeriodChangeStart(qint64 loop_period_ms);

    /**
     * Cancel the loop. After this signal has come in, isActive() will return false
     * and triggerTxNow will not fire any longer. Loop activity will restart
     * when a onTxLoopPeriodChangeStart signal is again received.
     *
     * This is idempotent, it does no harm to call this on an idle loop.
     */
    void onLoopCancel();

private slots:
    void onTimer();
};

#endif
