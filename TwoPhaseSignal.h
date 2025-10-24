#ifndef TWOPHASESIGNAL_H
#define TWOPHASESIGNAL_H
#include <QObject>

/**
 * This is a humble helper class for Qt signals and slots when this mechanism is also
 * used to initialize the data.  In that case, we first do the "plumbing" phase,
 * where signals and slots are connected as appropriate.  After that has been completed,
 * signals are to be sent in a volley so that an required initial values are being set.
 */
class TwoPhaseSignal : public QObject {
    Q_OBJECT
public:
    TwoPhaseSignal();
    ~TwoPhaseSignal();

public slots:
    /**
     * Called when the plumbing has been completed.
     * The object should react by fireing its signals.
     */
    virtual void onPlumbingCompleted() const = 0;
};

#endif
