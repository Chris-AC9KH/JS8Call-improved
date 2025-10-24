#ifndef QDATETIMEEXPERIMENT_H
#define QDATETIMEEXPERIMENT_H

#include <QDateTime>
#include <QTimer>

/**
 * The documentation of QDateTime does not mention whether seconds since epoch
 *  are rounded or truncated from the milliseconds. Let us find out experimentally:
 */
class QDateTimeRoundingExperiment : public QObject {
    Q_OBJECT

private:
    QTimer experimentQDateTimer;
public:
    QDateTimeRoundingExperiment();

public slots:
    void printRounding();
};

#endif
