#include <QLoggingCategory>

#include "qDateTimeExperiment.h"

Q_DECLARE_LOGGING_CATEGORY(qdatetimeexperiment_js8)

QDateTimeRoundingExperiment::QDateTimeRoundingExperiment() {
    if (qdatetimeexperiment_js8().isDebugEnabled()) {
        experimentQDateTimer.setTimerType(Qt::PreciseTimer);
        experimentQDateTimer.setSingleShot(true);
        connect(&experimentQDateTimer, &QTimer::timeout, this, &QDateTimeRoundingExperiment::printRounding);

        qint64 nowMS = QDateTime::currentMSecsSinceEpoch();
        qint64 into_second = nowMS % 1000;
        if (into_second < 950)
            experimentQDateTimer.start(950 - into_second);
        else
            printRounding();
    }
}

void QDateTimeRoundingExperiment::printRounding() {
    QDateTime now{QDateTime::currentDateTimeUtc()};
    qCDebug(qdatetimeexperiment_js8)
        << "How does QDateTime round?"
        << "Without drift, now is " << now
        << "which translates to" << now.toMSecsSinceEpoch() << "milliseconds after epoch"
        << "which is truncated to" << now.toSecsSinceEpoch() << "milliseconds after epoch";
}

Q_LOGGING_CATEGORY(qdatetimeexperiment_js8, "qdatetimeexperiment.js8", QtWarningMsg)
