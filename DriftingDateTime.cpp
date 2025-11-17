#include <QMutexLocker>
#include <QLoggingCategory>
#include <QThread>

#include "DriftingDateTime.h"
#include "qDateTimeExperiment.h"

Q_DECLARE_LOGGING_CATEGORY(driftingdatetime_js8)

static QPointer<QDateTimeRoundingExperiment> experiment;

DriftingDateTimeSingleton & DriftingDateTimeSingleton::getSingleton() {
    if (singleton.isNull()) {
        singleton = QPointer{new DriftingDateTimeSingleton{}};
        if (experiment.isNull()) {
            experiment = QPointer{new QDateTimeRoundingExperiment};
        }
    }
    return *(singleton.data());
}

DriftingDateTimeSingleton::DriftingDateTimeSingleton(): driftMS(0) {}

qint64 DriftingDateTimeSingleton::drift() const {
    QMutexLocker locker(&mutex);
    return driftMS;
}

void DriftingDateTimeSingleton::setDriftInner(qint64 ms) {
    QMutexLocker locker(&mutex);
    driftMS = ms;
}

void DriftingDateTimeSingleton::setDrift(qint64 ms) {
    qint64 old_drift = drift();
    setDriftInner(ms);
    if(ms != old_drift) {
        qCDebug(driftingdatetime_js8) << "Changed drift from" << old_drift << "to" << ms << "ms";
        emit driftChanged(ms);
    } else {
        qCDebug(driftingdatetime_js8) << "Incoming signal without change of drift, still" << old_drift << "ms";
    }
}

void DriftingDateTimeSingleton::onPlumbingCompleted() const {
    emit driftChanged(drift());
}

QPointer<DriftingDateTimeSingleton> DriftingDateTimeSingleton::singleton = QPointer<DriftingDateTimeSingleton>{};

Q_LOGGING_CATEGORY(driftingdatetime_js8, "driftingdatetime.js8", QtWarningMsg)
