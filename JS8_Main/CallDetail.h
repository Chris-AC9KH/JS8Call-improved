/**
 * @file CallDetail.h
 * @brief Defines the Call Activity record shared by the UI and storage.
 */
#pragma once

#include "JS8_Main/Radio.h"

#include <QDateTime>
#include <QString>

/**
 * @brief One station in the Call Activity table.
 *
 * Lives outside mainwindow.h so that the storage layer (ActivityDB and
 * ActivityStorageController) can convert to and from it without pulling
 * the whole window in. UI_Constructor aliases it, so it is still spelled
 * CallDetail everywhere it was before.
 */
struct CallDetail {
    QString call;
    QString through;
    QString grid;
    Radio::Frequency dial;
    int offset;
    QDateTime cqTimestamp;
    QDateTime ackTimestamp;
    QDateTime utcTimestamp;
    int snr;
    int bits;
    float tdrift;
    int submode;
};
