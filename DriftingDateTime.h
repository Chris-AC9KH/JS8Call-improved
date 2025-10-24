#ifndef DRIFTINGDATETIME_H
#define DRIFTINGDATETIME_H

#include <QDateTime>
#include <QMutex>
#include <QPointer>
#include "TwoPhaseSignal.h"

/**
 * JS8Call allows the user to manipulate the clock.
 *
 * One intention is: If a QSO partner's clock is off too much, we can
 * set our clock to correspond to that partner's clock.  This increases
 * the probability of successful decodes on both sides.
 *
 * Alternatively, we may not be able to set our own computer's clock
 * with appropriate precision.  If so, we can manipulate our
 * JS8Call-internal clock, e.g., based on incoming JS8 signals, to fit
 * other folks' clocks.
 *
 * There is only one, central clock for the entire application.
 *
 * The manipulated clock is ubiquitously used throughout JS8Call.  This
 * includes timestamps logged to ALL.TXT or via Qt logging.  This makes
 * it easier to judge, especially when reading the Qt logs, whether
 * JS8Call's timing is as it should be.
 *
 * There is exactly one object of class DriftingDateTimeObject
 * (singleton pattern).
 *
 * Drift specified as a qint64 is always in ms.
 * A positive drift means that the JS8Call internal clock
 * is that many milliseconds later than the system clock,
 * a negative drift that many milliseconds earlier.
 *
 * This functionality is (intended to be) thread-safe,
 * with the exception of the setDrift(), which must be called
 * by the same thread that originally constructed the object.
 * If you want to make sure this is not violated,
 * send the new drift value as a signal.
 **/
class DriftingDateTimeSingleton: public TwoPhaseSignal
{
Q_OBJECT

private:
    // As this is a subclass of QObject,
    // it lives in a thread: Whatever thread
    // first called getSingleton().
    DriftingDateTimeSingleton();
    qint64 driftMS;
    mutable QMutex mutex;

    static QPointer<DriftingDateTimeSingleton> singleton;

private:
    /**
     * This needs to be called by the same thread that constructed the object.
     */
    void setDriftInner(qint64 ms);

public:
    /**
     * The first thread that calls this is the thread the singleton lives in.
     */
    static DriftingDateTimeSingleton & getSingleton();

    /**
     * Retrive drift, in milliseconds.
     *
     * Positive values indicate the drifted clock is behind the system clock,
     * negative, it is early.
     */
    qint64 drift() const;

    /** Retrieve drifted "now" as UTC. */
    inline QDateTime currentDateTimeUtc() const {
        return QDateTime::currentDateTimeUtc().addMSecs(drift());
    }

    /** Retrieve drifted "now" as local time. */
    inline QDateTime currentDateTimeLocal() const {
        return QDateTime::currentDateTime().addMSecs(drift());
    }

    /** Retrieve drifted "now" as milliseconds since epoch. */
    inline qint64 currentMSecsSinceEpoch() const {
        return QDateTime::currentMSecsSinceEpoch() + drift();
    }

    /**
      * Retrieve drifted "now" as seconds since epoch.
      *
      * As we found out experimentally, QDateTime does not round,
      * but truncates.  So we do that, too.  In other words,
      * the number returned is the number of fully elapsed
      * seconds since the epoch.
      */
    inline qint64 currentSecsSinceEpoch() const {
        return currentMSecsSinceEpoch() / 1000;
    }

public slots:
    /** Set the drift. */
    void setDrift(qint64 ms);
    /** Emits to the driftChanged signal (as per TwoPhaseSignal contract). */
    void onPlumbingCompleted() const;

signals:
    /**
     * Communicate the drift change to anyone who want to know.
     *
     * This outgoing signal is the main motivation
     * for introducing this class in the first place,
     * rather than just using static methods.
     */
    void driftChanged(qint64 new_drift) const;
};


/** This namespace contains some static convenience functions. */
namespace DriftingDateTime
{
    inline qint64 drift() {
        return DriftingDateTimeSingleton::getSingleton().drift();
    }

    /**
     * This must only be called from whatever thread caused the first getSingleton
     * to be called.  If unsure, it is preferred to send a signal to
     * object &DriftingDateTimeSingleton::getSingleton(), slot &DriftingDateTimeSingleton::setDrift .
     */
    inline void setDrift(qint64 ms) {
        DriftingDateTimeSingleton::getSingleton().setDrift(ms);
    }

    inline QDateTime currentDateTimeUtc() {
        return DriftingDateTimeSingleton::getSingleton().currentDateTimeUtc();
    }

    inline QDateTime currentDateTimeLocal() {
        return DriftingDateTimeSingleton::getSingleton().currentDateTimeLocal();
    }

    inline qint64 currentMSecsSinceEpoch() {
        return DriftingDateTimeSingleton::getSingleton().currentMSecsSinceEpoch();
    }

    inline qint64 currentSecsSinceEpoch() {
        return DriftingDateTimeSingleton::getSingleton().currentSecsSinceEpoch();
    }
};

#endif // DRIFTINGDATETIME_H
